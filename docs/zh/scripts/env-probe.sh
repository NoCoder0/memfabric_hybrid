#!/bin/bash
set -euo pipefail
exec python3 "$(dirname "$(readlink -f "$0")")/1825-env-probe.py" "$@"
