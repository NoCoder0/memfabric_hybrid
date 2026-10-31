# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

"""Command-line for memfabric-hybrid"""

import argparse
import logging
import shutil
import subprocess
import sys
from pathlib import Path

from . import mem_scan
from ._acc_offload_build import ExtendBuildError as _ExtendBuildError
from ._acc_offload_build import acc_offload_info as _acc_offload_info
from ._acc_offload_build import ensure_acc_offload_ext as _ensure_acc_offload_ext
from ._acc_offload_build import uninstall_acc_offload_ext as _uninstall_acc_offload_ext
from .env_utils import get_soc_key as _get_soc_key

LOGGER = logging.getLogger(__name__)


def _show_version(_args: argparse.Namespace) -> int:
    version_file = Path(__file__).resolve().parent / "VERSION"
    try:
        sys.stdout.write(version_file.read_text(encoding="utf-8"))
    except OSError as error:
        LOGGER.error("Failed to read version file, path=%s, error=%s", version_file, error)
        return 1
    return 0


def _install_acc_offload(soc_key=None) -> int:
    """Build and install the acc_offload extend library after kernel ops install.

    Any failure fails the whole kernel install. Only the "environment does not
    support acc_offload (SoC/CANN/torch)" skip counts as success. All progress
    and skip messages are echoed by _acc_offload_build itself.
    """
    try:
        _ensure_acc_offload_ext(soc_key)
    except _ExtendBuildError as exc:
        LOGGER.error("failed to install acc_offload operator extend library: %s", exc)
        return 1
    except OSError as exc:
        LOGGER.error("failed to install acc_offload operator extend library, unexpected OS error: %s", exc)
        return 1
    return 0


def _manage_kernel(args: argparse.Namespace) -> int:
    if args.command == "version":
        return _show_version(args)

    bash = shutil.which("bash")
    if bash is None:
        LOGGER.error("bash is required to install kernel ops.")
        return 1

    installer = Path(__file__).resolve().parent / "_kernel" / "hybm" / "install.sh"
    if not installer.is_file():
        LOGGER.error(
            "kernel ops installer is missing, path=%s. Build the wheel in a valid CANN environment.", installer
        )
        return 1

    soc_key = None
    if args.command == "install":
        soc_key = args.soc_version or _get_soc_key()

    command = [bash, str(installer), f"--{args.command}"]
    if args.command == "install" and args.install_for_all:
        command.append("--install-for-all")
    exit_code = subprocess.run(command, check=False).returncode
    if exit_code != 0:
        return exit_code
    if args.command == "install":
        return _install_acc_offload(soc_key)
    if args.command == "uninstall":
        _uninstall_acc_offload_ext()
    if args.command == "info":
        _acc_offload_info()
    return 0


def _scan_memory(args: argparse.Namespace) -> int:
    mem_scan.show(args.node, args.min_mb, args.workers)
    return 0


def _collect_logs(args: argparse.Namespace) -> int:
    bash = shutil.which("bash")
    if bash is None:
        LOGGER.error("failed to collect logs: bash is not available, command=logs")
        return 1

    collector = Path(__file__).resolve().parent / "logs" / "collect_logs.sh"
    if not collector.is_file():
        LOGGER.error("failed to collect logs: collector script is missing, path=%s", collector)
        return 1

    command = [bash, str(collector), *args.collector_args]
    try:
        return subprocess.run(command, check=False).returncode
    except OSError as error:
        LOGGER.error("failed to start log collector, path=%s, error=%s", collector, error)
        return 1


def _register_version_command(components) -> None:
    version = components.add_parser("version", help="show version information")
    version.set_defaults(handler=_show_version)


def _register_kernel_command(components) -> None:
    kernel = components.add_parser("kernel", help="manage kernel")
    kernel.set_defaults(handler=_manage_kernel)
    commands = kernel.add_subparsers(dest="command", required=True)

    install = commands.add_parser("install", help="install kernel ops into CANN")
    install.add_argument(
        "--install-for-all",
        action="store_true",
        help="make the installed files readable by all users",
    )
    install.add_argument(
        "--soc-version",
        choices=("A2", "A3", "A5"),
        default=None,
        help="target SoC generation; skip auto detection when given (default: auto detect)",
    )
    commands.add_parser("info", help="show kernel ops install paths and CANN load configuration")
    commands.add_parser("version", help="show installed kernel version")
    commands.add_parser("uninstall", help="uninstall kernel")


def _register_mem_command(components) -> None:
    mem = components.add_parser("mem", help="manage memory")
    commands = mem.add_subparsers(dest="command", required=True)
    scan = commands.add_parser("scan", help="scan NUMA free memory")
    mem_scan.add_arguments(scan)
    scan.set_defaults(handler=_scan_memory)


def _register_logs_command(components) -> None:
    logs = components.add_parser("logs", help="(experimental) collect troubleshooting logs", add_help=False)
    logs.set_defaults(handler=_collect_logs)


def _run_benchmark(args: argparse.Namespace) -> int:
    from memfabric_hybrid.benchmark.benchmark import run as _benchmark_run

    return _benchmark_run(args)


def _run_trace(args: argparse.Namespace) -> int:
    from memfabric_hybrid.trace import ptracer_report

    return ptracer_report.report(args)


def _register_trace_command(components) -> None:
    try:
        from memfabric_hybrid.trace import ptracer_report
    except ImportError as error:
        # Registration runs before any handler, so a missing trace package must
        # not make the other subcommands unusable.
        LOGGER.warning("mfcli trace subcommand is unavailable: %s", error)
        return

    trace = components.add_parser(
        "trace",
        help="(experimental) print the hourly batch_read / batch_write table of one ptracer dump file",
    )
    ptracer_report.add_arguments(trace)
    trace.set_defaults(handler=_run_trace)


def _register_benchmark_command(components) -> None:
    benchmark = components.add_parser("benchmark", help="(experimental) run pairwise BM bandwidth benchmark")
    benchmark.set_defaults(handler=_run_benchmark)
    benchmark.add_argument(
        "target",
        nargs="?",
        choices=("bm", "trans", "write", "read"),
        help="API backend (default: bm), or write/read using the default backend",
    )
    benchmark.add_argument("operation", nargs="?", choices=("write", "read"), help="select write or read directions")
    benchmark.add_argument("--backend", choices=("bm", "trans"), help="compatibility option for the API backend")
    benchmark.add_argument(
        "--local-ip", help="Trans local control IP (default: route to --ip); each engine uses its own port"
    )

    benchmark.add_argument(
        "--direction",
        type=str,
        default=None,
        help="all, write, read, or a comma list (default: BM all; Trans D2rD,D2rH,rD2D,rH2D)",
    )
    benchmark.add_argument(
        "--device",
        type=str,
        default=None,
        help="NPU devices to use, e.g. 0,1 or 0-3 (logical ids under ASCEND_RT_VISIBLE_DEVICES; default: all visible)",
    )
    benchmark.add_argument(
        "--protocol",
        choices=("auto", "sdma", "device_rdma", "device_urma", "host_rdma", "host_urma", "host_tcp"),
        default="auto",
        help="communication protocol (default: auto; A2=device_rdma, A3=sdma, A5=device_urma)",
    )
    benchmark.add_argument(
        "--ip",
        type=str,
        default="tcp://127.0.0.1:12050",
        help="config store, tcp://<ip>:<port>; port + 1 is used for synchronization. A non-loopback address runs "
        "cross-machine: give the same --ip on both machines, the one that owns it hosts the store "
        "(default: tcp://127.0.0.1:12050, one machine)",
    )
    benchmark.add_argument(
        "--nic",
        "--rdma",
        dest="nic",
        type=str,
        default="tcp://0.0.0.0:10005",
        help="local transport NIC IP and port, format tcp://<ip>:<port> "
        "(--rdma is a compatibility alias; default: tcp://0.0.0.0:10005)",
    )
    benchmark.add_argument(
        "--block-size",
        type=str,
        default=None,
        help="block size, list or doubling range; multiples of 4K up to 1G (default: BM 16K:2M; Trans 16K:8M)",
    )
    transfer = benchmark.add_mutually_exclusive_group()
    transfer.add_argument(
        "--batch-size",
        type=int,
        default=None,
        help="blocks per copy (default: 128M divided by the block size, at least 1)",
    )
    transfer.add_argument(
        "--transfer-size", help="target bytes per batch, e.g. 128M or 1G (default: 128M); exclusive with --batch-size"
    )
    benchmark.add_argument("--copy-count", type=int, default=5, help="timed copies per block size (default: 5)")
    benchmark.add_argument(
        "--warmup-count", type=int, default=1, help="untimed copies before each measurement (default: 1)"
    )
    benchmark.add_argument(
        "--group",
        type=int,
        choices=(0, 1),
        default=None,
        help="optional override of the cross-machine side; by default a non-loopback --ip selects cross-machine mode "
        "and the machine that owns the --ip address is group 0, the other group 1 (needed only for two commands "
        "on one machine or when detection is wrong)",
    )
    benchmark.add_argument("--log", action="store_true", help="show runtime logs and per-rank details")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="mfcli")
    components = parser.add_subparsers(dest="component", required=True)
    _register_version_command(components)
    _register_kernel_command(components)
    _register_mem_command(components)
    _register_logs_command(components)
    _register_benchmark_command(components)
    _register_trace_command(components)
    return parser


def _parse_args(argv=None) -> argparse.Namespace:
    parser = _parser()
    args, remaining = parser.parse_known_args(argv)
    if args.component == "logs":
        args.collector_args = remaining
    elif remaining:
        parser.error(f"unrecognized arguments: {' '.join(remaining)}")
    if args.component == "benchmark":
        _resolve_benchmark_selectors(args, parser)
    return args


def _resolve_benchmark_selectors(args, parser):
    target = args.target
    operation = args.operation
    if target in ("write", "read"):
        if operation:
            parser.error(f"benchmark accepts only one operation: {target} {operation}")
        operation = target
    elif target:
        if args.backend and args.backend != target:
            parser.error(f"benchmark {target} conflicts with --backend {args.backend}")
        args.backend = target
    args.backend = args.backend or "bm"
    if operation:
        if args.direction and args.direction.lower() != operation:
            parser.error(f"benchmark {operation} conflicts with --direction {args.direction}")
        args.direction = operation


def main() -> int:
    logging.basicConfig(
        level=logging.INFO,
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
        datefmt="%Y-%m-%dT%H:%M:%S%z",
    )
    args = _parse_args()
    return args.handler(args)


if __name__ == "__main__":
    sys.exit(main())
