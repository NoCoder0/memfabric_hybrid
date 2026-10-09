# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# ZBAL is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.
import logging
import os
import glob
from pathlib import Path
import sysconfig
import subprocess
import platform
import shutil
import sys
import time
from concurrent.futures import ThreadPoolExecutor

from importlib.metadata import version
import setuptools
from setuptools import setup
from torch.utils.cpp_extension import CppExtension, BuildExtension
from wheel.bdist_wheel import bdist_wheel

import torch
import torch_npu

logger = logging.getLogger(__name__)
logging.basicConfig(level=logging.INFO)


def _find_ascend_home_dir():
    """
    Get the home dir of ascend for kernel compiling.
    The default path is:
    /usr/local/Ascend/ascend-toolkit/latest
    """
    env = os.environ.get("ASCEND_TOOLKIT_HOME")
    if env:
        return env
    default_dir = "/usr/local/Ascend/ascend-toolkit/latest"
    if os.path.isdir(default_dir):
        return default_dir
    alter = "/usr/local/Ascend/ascend-toolkit"
    latest = os.path.join(alter, "latest")
    return latest if os.path.isdir(latest) else default_dir


def _find_python_include():
    return sysconfig.get_path('include')


def _get_version(version_dir):
    with open(f"{version_dir}/VERSION", "r", encoding="utf-8") as f:
        return f.read().strip()


def _check_env_flag(name: str, default: str = "") -> bool:
    return os.getenv(name, default).upper() in ["ON", "1", "YES", "TRUE", "Y"]


# Chip variants that can be bundled into one wheel. SOC_VERSION selects the AscendC
# kernel compilation target of each variant; at runtime zbal/__init__.py detects the
# local chip and imports the matching zbal.zbal_<a3|a5> backend extension.
CHIP_VARIANT_CONFIG = {
    "A3": {"soc_version": "ascend910_9382"},
    "A5": {"soc_version": "ascend950pr_9599"},
}


def _get_chip_variants():
    """
    Parse the chip variant list from ZBAL_CHIP_TYPES (comma-separated, e.g. "A3", "A3,A5").
    Default builds both variants so a single wheel runs on either hardware.
    """
    raw = os.getenv("ZBAL_CHIP_TYPES", "A3,A5")
    variants = []
    for item in raw.split(","):
        chip = item.strip().upper()
        if not chip:
            continue
        if chip not in CHIP_VARIANT_CONFIG:
            raise ValueError(
                f"unsupported chip type '{chip}' in ZBAL_CHIP_TYPES='{raw}', supported: {sorted(CHIP_VARIANT_CONFIG)}"
            )
        if chip not in variants:
            variants.append(chip)
    if not variants:
        raise ValueError(f"ZBAL_CHIP_TYPES='{raw}' contains no valid chip type")
    return variants


def _run_cmd(cmd, cwd, tag):
    """Run cmd and prefix every output line with the variant tag, so interleaved output
    from concurrently building variants stays distinguishable in the build log. Start/end
    wall-clock stamps make variant overlap (parallel execution) visible in the log."""
    start_ts = time.time()
    logger.info(f"{tag} {cmd[0]} started at {time.strftime('%H:%M:%S')}")
    with subprocess.Popen(
        cmd, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, universal_newlines=True
    ) as proc:
        for line in proc.stdout:
            print(f"{tag} {line}", end="", flush=True)
        # returncode is set only after wait(): reading it right after the stdout loop
        # (pipe EOF) still yields None, falsely failing builds whose cmake/make exited 0.
        proc.wait()
        logger.info(f"{tag} {cmd[0]} finished at {time.strftime('%H:%M:%S')} in {time.time() - start_ts:.0f}s")
        return proc.returncode


is_manylinux = _check_env_flag("IS_MANYLINUX", "FALSE")
is_debug_mode = _check_env_flag("DEBUG_MODE", "FALSE")
build_ut = _check_env_flag("ENABLE_ZBAL_UT", "OFF")
build_filter = os.getenv("ZBAL_OP_BUILD_FILTER", "").strip()
_op_aliases = {"dcn": "dispatch_combine_normal", "dcll": "dispatch_combine_low_latency", "fmoe": "fused_deep_moe"}
filter_ops = [_op_aliases.get(op.strip(), op.strip()) for op in build_filter.split(",") if op.strip()]
fused_deep_moe_enabled = not filter_ops or "fused_deep_moe" in filter_ops

chip_variants = _get_chip_variants()

ascend_home = Path(_find_ascend_home_dir()).resolve()
python_include_dir = Path(_find_python_include()).resolve()
torch_dir = Path(os.path.dirname(torch.__file__)).resolve()
torch_npu_dir = Path(os.path.dirname(torch_npu.__file__)).resolve()
torch_npu_version = version('torch_npu')
repo_root = Path(__file__).parent.parent.parent.parent.parent  # memfabric_hybrid repo root
zbal_root = repo_root / "app/zbal/"
package_version = _get_version(version_dir=repo_root)
cur_dir = os.path.dirname(os.path.abspath(__file__))


# allocator compile inputs
include_dirs = [
    f"{zbal_root}/",
    f"{zbal_root}/third_party/ska",
    f"{zbal_root}/third_party/mstx",
    f"{zbal_root}/src/include",
    f"{zbal_root}/src/csrc/",
    f"{zbal_root}/src/csrc/operators",
    f"{zbal_root}/src/csrc/operators/npu",
    f"{zbal_root}/src/csrc/common",
    f"{zbal_root}/src/csrc/sma",
    f"{zbal_root}/src/csrc/under_api/cann",
    f"{zbal_root}/src/csrc/under_api/memfabric",
    f"{zbal_root}/src/csrc/bootstrap",
    f"{zbal_root}/src/csrc/bootstrap/memory",
    f"{zbal_root}/src/csrc/bootstrap/memory/memfabric",
    f"{zbal_root}/src/csrc/adaptor/pytorch_npu/",
    f"{zbal_root}/src/csrc/adaptor/deepep/",
]


def _variant_include_dirs(chip):
    dirs = list(include_dirs)
    if chip == "A3" and fused_deep_moe_enabled:
        dirs.append(f"{zbal_root}/src/csrc/operators/npu/host/fused_deep_moe")
    return dirs


def _variant_define_macros(chip):
    # Mirrors CMakeLists.txt: `ZBAL_ASCEND_NPU_A3 AND ZBAL_BUILD_OP_FUSED_DEEP_MOE` defines the
    # same macro for the kernel archives; the pybind registration in zbal_deepep*.cpp is guarded
    # by it, so both sides must stay in sync or the extension fails to link/load.
    macros = [(f"ZBAL_ASCEND_NPU_{chip}", "1")]
    if chip == "A3" and fused_deep_moe_enabled:
        macros.append(("ZBAL_FUSED_DEEP_MOE_ENABLED", "1"))
    return macros


base_library_dirs = [
    f"{torch_dir}/lib",
    f"{torch_npu_dir}/lib",
    f"{ascend_home}/lib64",
    sysconfig.get_config_var("LIBDIR"),
]


csrc_dir = repo_root / "app" / "zbal" / "src" / "csrc"
sources = (
    [f"{csrc_dir}/zbal_pybind.cpp"]
    + glob.glob(str(csrc_dir / "sma" / "*.cpp"))
    + glob.glob(str(csrc_dir / "adaptor" / "pytorch_npu" / "*.cpp"))
    + glob.glob(str(csrc_dir / "adaptor" / "deepep" / "*.cpp"))
)


libraries = ["torch", "torch_npu", "c10", "torch_python", "opapi", "zbal_core", "zbal_kernel"]


logger.warning(f"Using ASCEND_TOOLKIT_HOME at: {ascend_home}")
logger.warning(f"{chip_variants=}")
logger.warning(f"{include_dirs=}")
logger.warning(f"{sources=}")
logger.warning(f"{base_library_dirs=}")
logger.warning(f"{libraries=}")


extra_compile_args = [
    "-std=c++17",
    "-Wno-unused-parameter",
    "-lno-unused-function",
    "-Wno-unused-function",
    "-Wunused-value",
    "-Wcast-align",
    "-Wcast-qual",
    "-Winvalid-pch",
    "-Wwrite-strings",
    "-Wsign-compare",
    "-Wextra",
    "-O3",
    "-fvisibility-inlines-hidden",
    "-fstack-protector-strong",
    "-Wl,-z,noexecstack",
    "-Wl,-z,relro",
    "-Wl,-z,now",
    "-fPIE",
    "-fPIC",
    "-ftrapv",
    "-isystem",
    f"{python_include_dir}",
    "-isystem",
    f"{ascend_home}/include",
    "-isystem",
    f"{ascend_home}/include/experiment/runtime/runtime/",
    "-isystem",
    f"{torch_dir}/",
    "-isystem",
    f"{torch_dir}/include/torch/csrc/api/include/",
    "-isystem",
    f"{torch_dir}/include/torch/csrc/utils/",
    "-isystem",
    f"{torch_dir}/include/c10/util/",
    "-isystem",
    f"{torch_dir}/include/c10/core/",
    "-isystem",
    f"{torch_dir}/include/",
    "-isystem",
    f"{torch_dir}/include/ATen/",
    "-isystem",
    f"{torch_dir}/include/ATen/detail/",
    "-isystem",
    f"{torch_dir}/include/torch/csrc/distributed/",
    "-isystem",
    f"{torch_dir}/include/torch/csrc/distributed/c10d/",
    "-isystem",
    f"{torch_npu_dir}/include/",
    "-isystem",
    f"{torch_npu_dir}/include/torch_npu/csrc/aten/",
    "-isystem",
    f"{torch_npu_dir}/include/torch_npu/csrc/core/npu/",
]  # "-fvisibility=hidden"

# Strip at link time in release: CANN's ascendc.cmake compiles host code with -g, which
# otherwise adds ~50MB of DWARF to each .so. .dynsym is kept for pybind/ctypes lookups.
extra_link_args = [] if is_debug_mode else ["-Wl,-s"]

VERSION_FILE = "VERSION"


def version_file_path():
    return os.path.join(cur_dir, "zbal", VERSION_FILE)


def get_git_commit():
    try:
        result = subprocess.run(["git", "rev-parse", "HEAD"], cwd=repo_root, capture_output=True, text=True)
    except OSError as err:
        logger.warning(f"failed to run git for commit id: {err}")
        return "unknown"
    if result.returncode != 0:
        logger.warning(f"git rev-parse HEAD failed with code {result.returncode}: {result.stderr.strip()}")
        return "unknown"
    return result.stdout.strip()


def write_version_file():
    """Write into the package source so the built wheel bundles it at site-packages/zbal/VERSION."""
    version_file = version_file_path()
    lines = [
        f"zbal version: {package_version}",
        f"commit id: {get_git_commit()}",
        f"pta (torch_npu) version: {torch_npu_version}",
        f"torch version: {torch.__version__}",
        f"build time: {time.strftime('%Y-%m-%d %H:%M:%S')}",
    ]
    content = os.linesep.join(lines) + os.linesep
    try:
        with open(version_file, "wb") as f:
            f.write(content.encode("utf-8"))
    except OSError as err:
        logger.error(f"failed to write version file {version_file}: {err}")
        raise
    logger.info(f"version info written to {version_file}: pta={torch_npu_version}")


def remove_version_file():
    version_file = version_file_path()
    if not os.path.exists(version_file):
        return
    try:
        os.remove(version_file)
    except OSError as err:
        logger.warning(f"failed to remove generated version file {version_file}: {err}")
    else:
        logger.info(f"removed generated {version_file}")


write_version_file()


class CustomBuildExtension(BuildExtension):
    def build_base_zbal(self):
        root_dir = Path(cur_dir).parent.parent
        # Wipe the whole build/output trees: they are pure artifacts, and a stale parent
        # build dir (e.g. CMakeCache.txt left by old single-variant builds) can break
        # nested per-variant cmake invocations.
        shutil.rmtree(os.path.join(f"{root_dir}", "build"), ignore_errors=True)
        shutil.rmtree(os.path.join(f"{root_dir}", "output"), ignore_errors=True)
        # Variants are fully isolated (own build/output dirs, own SOC_VERSION and chip
        # macros), so compile them concurrently: with N variants the kernel compilation
        # wall time drops from sum(T_i) to max(T_i).
        with ThreadPoolExecutor(max_workers=len(chip_variants)) as pool:
            futures = [pool.submit(self.build_zbal_variant, root_dir, chip) for chip in chip_variants]
            for future in futures:
                future.result()

    def build_zbal_variant(self, root_dir, chip):
        chip_lower = chip.lower()
        tag = f"[{chip}]"
        build_dir = os.path.join(f"{root_dir}", "build", chip_lower)
        output_dir = os.path.join(f"{root_dir}", "output", chip_lower)
        os.makedirs(build_dir, exist_ok=True)
        os.makedirs(output_dir, exist_ok=True)
        logger.info(f"{tag} make build dir:{build_dir}")
        logger.info(f"{tag} make output dir:{output_dir}")

        # cmake
        # Uppercase is mandatory: CMakeLists.txt matches `CMAKE_BUILD_TYPE MATCHES "RELEASE"`
        # case-sensitively, so "Release" would silently fall into the -g -rdynamic branch.
        build_type = "DEBUG" if is_debug_mode else "RELEASE"

        cmake_cmd = [
            "cmake",
            # Source dir must be explicit: with a nested build dir (build/<chip>) the relative
            # ".." resolves to the stale parent build dir (holding an old CMakeCache.txt from
            # single-variant builds), which cmake silently reuses and writes files to.
            f"{root_dir}",
            f"-DSOC_VERSION={CHIP_VARIANT_CONFIG[chip]['soc_version']}",
            f"-DZBAL_ASCEND_NPU_{chip}=1",
            f"-DBUILD_ZBAL_MODULE_UT={build_ut}",
            f"-DCMAKE_BUILD_TYPE={build_type}",
            f"-DZBAL_LIB_OUTPUT_SUBDIR={chip_lower}",
            "-DDISABLE_ADAPTOR_COMPILE=ON",
            "-DDISABLE_ALLOCATOR_COMPILE=ON",
            "-DDISABLE_SHARE_COMPILE=ON",
        ]
        if build_filter:
            canonical_filter = ",".join(filter_ops)
            cmake_cmd.append(f"-DZBAL_OP_BUILD_FILTER={canonical_filter}")
            logger.info(f"{tag} operator build filter enabled: {canonical_filter}")
        ret = _run_cmd(cmake_cmd, cwd=build_dir, tag=tag)
        if ret != 0:
            logger.error(f"{tag} python cmake exec failed ret code {ret}")
            raise RuntimeError(f"cmake exec failed for {chip} variant")
        logger.info(f"{tag} python cmake exec success")

        # make
        make_cmd = ["make", f"-j16"]
        ret = _run_cmd(make_cmd, cwd=build_dir, tag=tag)
        if ret != 0:
            logger.error(f"{tag} python make exec failed ret code {ret}")
            raise RuntimeError(f"make exec failed for {chip} variant")
        logger.info(f"{tag} python make exec success")

        # copy
        static_output = glob.glob(f"{build_dir}/**/*.a", recursive=True)
        for x in static_output:
            static_name = os.path.basename(x)
            dst = f"{output_dir}/{static_name}"
            shutil.copy2(x, dst)
            logger.info(f"{tag} copy {x} to {dst}")

    def run(self):
        self.build_base_zbal()
        super().run()


class BuildWheel(bdist_wheel):
    def run(self):
        bdist_wheel.run(self)

        exclude_libraries = [
            # torch
            "libtorch.so",
            "libtorch_npu.so",
            "libc10.so",
            "libtorch_python.so",
            "libtorch_cpu.so",
            "libopapi.so",
        ]

        if is_manylinux:
            file = glob.glob(os.path.join(self.dist_dir, "*-linux_*.whl"))[0]

            auditwheel_cmd = [
                "auditwheel",
                "-v",
                "repair",
                "--plat",
                f"manylinux_2_34_{platform.machine()}",
                "-w",
                self.dist_dir,
                file,
            ]
            for lib in exclude_libraries:
                auditwheel_cmd.extend(["--exclude", lib])

            try:
                subprocess.run(auditwheel_cmd, check=True, stdout=subprocess.PIPE)
            finally:
                os.remove(file)


setup(
    name="memfabric_zbal",
    version=package_version,
    description="ZBAL pronounced [zi:bəl], stands for Zero Buffer Acceleration Library. "
    "It contains a bunch of well tuned operators for LLM inference and training, "
    "which has two key advantages: zero intermediate buffer and blazing fast.",
    url="https://gitcode.com/Ascend/memfabric_hybrid/app/zbal",
    author="Huawei",
    license="Mulan PSL v2",
    ext_modules=[
        CppExtension(
            name=f"zbal.zbal_{chip.lower()}",  # TORCH_EXTENSION_NAME; .so lands directly in zbal/, no nested package needed
            sources=sources,
            include_dirs=_variant_include_dirs(chip),
            library_dirs=[*base_library_dirs, f"{zbal_root}/output/{chip.lower()}"],
            libraries=libraries,
            define_macros=_variant_define_macros(chip),
            extra_compile_args=extra_compile_args,
            extra_link_args=extra_link_args,
            cxx_std=17,
        )
        for chip in chip_variants
    ],
    python_requires=">=3.10",
    packages=setuptools.find_packages(include=["zbal", "zbal.*"]),
    package_data={"zbal": [VERSION_FILE]},
    cmdclass={'build_ext': CustomBuildExtension, 'bdist_wheel': BuildWheel},
)

remove_version_file()
