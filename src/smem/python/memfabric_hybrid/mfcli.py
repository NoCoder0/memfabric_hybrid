#!/usr/bin/env python3
"""Command-line installer for the HYBM AICPU OPS payload."""

import argparse
import logging
import shutil
import subprocess
import sys
from pathlib import Path


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="mfcli")
    components = parser.add_subparsers(dest="component", required=True)
    aicpu = components.add_parser("aicpu", help="manage HYBM AICPU OPS")
    commands = aicpu.add_subparsers(dest="command", required=True)

    install = commands.add_parser("install", help="install HYBM OPS into CANN")
    install.add_argument("--force", action="store_true", help="overwrite an existing installation")
    install.add_argument(
        "--install-for-all",
        action="store_true",
        help="make the installed files readable by all users",
    )
    commands.add_parser("uninstall", help="uninstall HYBM OPS")
    commands.add_parser("info", help="show HYBM OPS install paths and CANN load configuration")
    return parser


def main() -> int:
    logging.basicConfig(format="%(message)s")
    args = _parser().parse_args()
    bash = shutil.which("bash")
    if bash is None:
        logging.error("Error: bash is required to install HYBM OPS.")
        return 1

    installer = Path(__file__).resolve().parent / "memfabric_hybrid" / "_aicpu" / "install.sh"
    if not installer.is_file():
        logging.error(
            "Error: this wheel does not contain HYBM OPS artifacts. Build the wheel in a valid CANN environment."
        )
        return 1

    command = [bash, str(installer), f"--{args.command}"]
    if args.command == "install":
        if args.force:
            command.append("--force")
        if args.install_for_all:
            command.append("--install-for-all")
    return subprocess.run(command, check=False).returncode


if __name__ == "__main__":
    sys.exit(main())
