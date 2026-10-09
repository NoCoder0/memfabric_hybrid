import importlib
import logging
import os
import sys
import types
from importlib.metadata import version

import torch
import torch_npu

_RT_MAJOR_VER_MIN_ = 9
logger = logging.getLogger(__name__)
logging.basicConfig(level=logging.INFO)


def _read_build_version_info():
    """Read the build-time VERSION file bundled in the installed package."""
    version_file = os.path.join(os.path.dirname(os.path.abspath(__file__)), "VERSION")
    info = {}
    with open(version_file, "r", encoding="utf-8") as f:
        for line in f:
            key, sep, value = line.partition(":")
            if sep:
                info[key.strip()] = value.strip()
    return info


try:
    # check pta version against the build-time version recorded in the bundled VERSION file
    build_info = _read_build_version_info()
    exp_torch_npu_version = build_info.get("pta (torch_npu) version")
    if not exp_torch_npu_version:
        raise ValueError(f"[ERROR] pta version not found in VERSION file, build_info={build_info}")
    cur_torch_npu_version = version('torch_npu')
    cur_major, cur_minor, *_ = cur_torch_npu_version.split(".")
    exp_major, exp_minor, *_ = exp_torch_npu_version.split(".")
    if cur_major != exp_major or cur_minor != exp_minor:
        raise ValueError(f"[ERROR] Expect torch_npu version {exp_torch_npu_version} but found {cur_torch_npu_version}")

    # check cann version
    ascend_home_path = os.environ.get("ASCEND_HOME_PATH", "un-defined env ASCEND_HOME_PATH")
    runtime_version_file = f"{ascend_home_path}/share/info/runtime/version.info"
    version_line = next((line.strip() for line in open(runtime_version_file) if 'Version=' in line), None)
    _, cur_rt_version = version_line.split("=")
    cur_rt_major_version, *_ = cur_rt_version.split(".")
    if int(cur_rt_major_version) < _RT_MAJOR_VER_MIN_:
        logger.warning(f"[WARNING] The CANN version = {cur_rt_version} is too low, expect version is 9.0.0 or higher.")
except Exception as e:
    logger.exception(f"[ERROR] zbal init module failed: {e}")
    raise e

# ---- Load the chip-variant pybind backend (one wheel bundles zbal_a3 / zbal_a5) ----
# The variant matching the local chip is imported and aliased under the legacy `zbal.zbal`
# namespace, so existing `zbal.zbal` / `zbal.zbal.<submodule>` import paths keep working.


def _query_soc_name_via_acl():
    """Query the SoC name (e.g. 'Ascend910_93' / 'Ascend950') through the Python ACL binding.

    MUST NOT use torch.npu device APIs here: they trigger torch_npu's lazy init and
    mark its allocator initialized, which would later break switch_to_allocator()
    with 'Can't swap an already initialized allocator'. The standalone `acl` package
    does not touch torch_npu's allocator state.
    """
    try:
        import acl

        soc = acl.get_soc_name()
    except Exception:
        return None
    return soc if isinstance(soc, str) and soc else None


def _detect_chip_type():
    """Detect the local NPU chip type: 'A5' for Ascend950/Ascend910_95, otherwise 'A3'."""
    soc_name = _query_soc_name_via_acl()
    if not soc_name:
        raise RuntimeError(
            "failed to detect the NPU SoC name via acl.get_soc_name(); check that the acl Python package "
            "is installed and the Ascend driver/runtime environment (ASCEND_HOME_PATH) is configured"
        )
    chip = "A5" if ("Ascend950" in soc_name or "Ascend910_95" in soc_name) else "A3"
    logger.info("detected SoC %r -> chip type %s", soc_name, chip)
    return chip


chip_type = _detect_chip_type()
try:
    backend = importlib.import_module(f"zbal.zbal_{chip_type.lower()}")
except ImportError as e:
    backend_name = f"zbal.zbal_{chip_type.lower()}"
    raise ImportError(
        f"failed to load zbal backend '{backend_name}' for chip type {chip_type}: the wheel may not bundle "
        f"this variant, rebuild with ZBAL_CHIP_TYPES={chip_type}"
    ) from e

sys.modules["zbal.zbal"] = backend
sys.modules[__name__].zbal = backend  # keep `zbal.zbal.<attr>` attribute-style access working
for _attr in dir(backend):
    _obj = getattr(backend, _attr)
    if isinstance(_obj, types.ModuleType):
        sys.modules[f"zbal.zbal.{_attr}"] = _obj
# Absolute path of the loaded variant .so, used by the pluggable allocator (zbal_module.ZBAL_LIB).
backend_lib_path = backend.__file__
logger.info("zbal backend loaded: %s (chip_type=%s)", backend.__name__, chip_type)

from zbal.zbal.allocator import record_memory_history, dump_snapshot, simulate_init, is_mix_alloc
from zbal.zbal import ProcessGroupZBAL
from zbal.zbal.deepep_adaptor import Config
from zbal.zbal import ZBALBootstrapType, zbal_set_logger_level

from .zbal_module import zbal_init, zbal_uninit
from .zbal_module import switch_to_allocator, zbal_get_symm_base_addr
from .zbal_module import mem_get_info
from .zbal_module import __version__

from .zbal_buffer import Buffer
from .zbal_utils import EventOverlap

__all__ = [
    "switch_to_allocator",
    "zbal_get_symm_base_addr",
    "record_memory_history",
    "dump_snapshot",
    "get_heap_stats",
    "zbal_init",
    "zbal_uninit",
    "ZBALBootstrapType",
    "zbal_set_logger_level",
    "simulate_init",
    "is_mix_alloc",
    "__version__",
]


def _new_process_helper(dist_backend_opts, pg_options):
    store = dist_backend_opts.store
    group_rank = dist_backend_opts.group_rank
    group_size = dist_backend_opts.group_size

    if pg_options is None or not isinstance(pg_options, ProcessGroupZBAL.Options):
        pg_options = ProcessGroupZBAL.Options()

    pg_options.is_high_priority_stream = False
    pg_options.op_timeout = dist_backend_opts.timeout
    pg_options.global_ranks_in_group = dist_backend_opts.global_ranks_in_group
    pg_options.group_id = dist_backend_opts.group_id
    return ProcessGroupZBAL(store, group_rank, group_size, pg_options)


torch.distributed.Backend.register_backend(
    "zbal",
    lambda dist_backend_opts, pg_options: _new_process_helper(dist_backend_opts, pg_options),
    extended_api=True,
    devices=["npu"],
)
