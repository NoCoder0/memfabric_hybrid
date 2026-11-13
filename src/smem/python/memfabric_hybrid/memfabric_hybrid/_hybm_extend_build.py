# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""hybm extend kernel libraries (copy_extend + aiv_direct_sdma) build entry.

Compiles the AscendC extend kernels shipped inside the NPU wheel into loadable
.so libraries. Invoked via ``mfcli kernel install``; the environment must
provide CANN (bisheng compiler under ASCEND_HOME_PATH).
"""

import logging
import os
import shutil
import subprocess
import tempfile

from .env_utils import get_ascend_home as _get_ascend_home

_COPY_EXTEND_SRC = "_kernel/copy_extend/hybm_copy_kernel.cpp"
_COPY_EXTEND_LIB = "libmf_hybm_copy_extend.so"

_AIV_DIRECT_SDMA_SRC = "_kernel/copy_aiv_direct_sdma/aiv_direct_sdma_kernel.cpp"
_AIV_DIRECT_SDMA_LIB = "libmf_hybm_aiv_direct_sdma.so"

_BUILD_TIMEOUT_SEC = 300

_EXTEND_LIBS = {
    "copy_extend": (_COPY_EXTEND_SRC, _COPY_EXTEND_LIB),
    "aiv_direct_sdma": (_AIV_DIRECT_SDMA_SRC, _AIV_DIRECT_SDMA_LIB),
}


class ExtendBuildError(RuntimeError):
    pass


def _log(level, msg):
    getattr(logging.getLogger(__name__), {"INFO": "info", "WARN": "warning", "ERROR": "error"}[level])(msg)


def _echo_header():
    print("\nMemFabric Hybrid Extend:")


def _get_pkg_dir():
    return os.path.dirname(os.path.abspath(__file__))


def _cache_root():
    return os.path.join(os.path.expanduser("~"), ".cache", "memfabric_hybrid", "extend")


def _resolve_target(pkg_dir):
    lib_dir = os.path.join(pkg_dir, "lib")
    if not os.access(lib_dir, os.W_OK):
        raise ExtendBuildError("extend install directory is not writable: %s" % lib_dir)
    return lib_dir


def _find_bisheng(ascend_home):
    bisheng = shutil.which("bisheng")
    if bisheng is not None:
        return bisheng
    candidate = os.path.join(ascend_home, "bin", "bisheng")
    if os.path.isfile(candidate):
        return candidate
    return None


def _compile_one(bisheng, src_path, out_name, arch, work_dir, timeout, extra_args=None):
    out = os.path.join(work_dir, out_name)
    cmd = [
        bisheng,
        "-x",
        "asc",
        src_path,
        "-fPIC",
        "-shared",
        "-g",
    ]
    if extra_args:
        cmd.extend(extra_args)
    cmd += [
        "-o",
        out,
        "--cce-aicore-arch=" + arch,
    ]
    try:
        subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, check=True)
    except subprocess.TimeoutExpired:
        raise ExtendBuildError("bisheng compilation timed out ({}s) for {}".format(timeout, out_name))
    except subprocess.CalledProcessError as exc:
        stderr = exc.stderr or ""
        raise ExtendBuildError("bisheng failed for {}: {}".format(out_name, stderr[-1024:]))
    if not os.path.isfile(out) or os.path.getsize(out) == 0:
        raise ExtendBuildError("output %s is empty or missing" % out)
    return out


def _install_one(src, dst_dir, lib_name):
    dst = os.path.join(dst_dir, lib_name)
    os.makedirs(dst_dir, exist_ok=True)
    shutil.copy2(src, dst)


def ensure_hybm_extend(soc_key, only=None):
    """Compile and install copy_extend / aiv_direct_sdma extend libraries."""
    ascend_home = _get_ascend_home()
    bisheng = _find_bisheng(ascend_home)
    if bisheng is None:
        _echo_header()
        _log("WARN", "bisheng is not found (PATH or $ASCEND_HOME_PATH/bin), skip extend build.")
        return None

    if soc_key is None:
        _echo_header()
        _log(
            "ERROR",
            "cannot detect ascend chip version, abort extend build. "
            "Ensure npu-smi is in PATH (source CANN set_env.sh) before running mfcli kernel install.",
        )
        raise ExtendBuildError("cannot detect ascend chip version")

    pkg_dir = _get_pkg_dir()
    target = _resolve_target(pkg_dir)

    to_build = []
    if only == "copy_extend":
        to_build.append("copy_extend")
    elif only == "aiv_direct_sdma":
        if soc_key != "A3":
            _echo_header()
            _log("INFO", "aiv_direct_sdma only supports A3 (detected %s), skipped." % soc_key)
            return None
        to_build.append("aiv_direct_sdma")
    else:
        to_build.append("copy_extend")
        if soc_key == "A3":
            to_build.append("aiv_direct_sdma")

    _echo_header()
    print("  Start installing, this may take minutes...")

    arch = "dav-c310" if soc_key == "A5" else "dav-c220"

    installed = []
    with tempfile.TemporaryDirectory(prefix="hybm_extend_") as tmp:
        for name in to_build:
            src_rel, lib_name = _EXTEND_LIBS[name]
            src = os.path.join(pkg_dir, src_rel)
            if not os.path.isfile(src):
                raise ExtendBuildError("extend source not found: %s" % src)
            extra_args = ["-O2"] if name == "aiv_direct_sdma" else None
            try:
                so = _compile_one(bisheng, src, lib_name, arch, tmp, _BUILD_TIMEOUT_SEC, extra_args)
            except ExtendBuildError as exc:
                if name == "aiv_direct_sdma":
                    _log(
                        "WARN",
                        "install AIV Direct SDMA library failed; "
                        "Host SDMA and other transports remain available: %s" % exc,
                    )
                    continue
                raise
            _install_one(so, target, lib_name)
            installed.append(name)

    for name in installed:
        print("  Install done. Path:\n    %s/%s" % (target, _EXTEND_LIBS[name][1]))
    return target


def info_hybm_extend():
    """Print extend library status. Never raises."""
    _echo_header()
    pkg_dir = _get_pkg_dir()
    found = False
    for search in [os.path.join(pkg_dir, "lib"), _cache_root()]:
        for name, (_, lib) in _EXTEND_LIBS.items():
            path = os.path.join(search, lib)
            if os.path.isfile(path) and os.path.getsize(path) > 0:
                print("  Installed. Path: %s" % path)
                found = True
    if not found:
        print("  Not installed. run 'mfcli kernel install' to build it")


def uninstall_hybm_extend():
    """Remove installed extend libraries. Never raises."""
    _echo_header()
    print("  Start uninstalling...")
    pkg_dir = _get_pkg_dir()
    for search in [os.path.join(pkg_dir, "lib"), _cache_root()]:
        for name, (_, lib) in _EXTEND_LIBS.items():
            path = os.path.join(search, lib)
            if os.path.isfile(path):
                try:
                    os.remove(path)
                except OSError as exc:
                    _log("WARN", "failed to remove %s: %s" % (path, exc))
    print("  Uninstall done.")
