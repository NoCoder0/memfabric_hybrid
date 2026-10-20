#!/bin/bash
# Copyright (c) Huawei Technologies Co., Ltd. 2026-2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

readonly SCRIPT_FULL_PATH=$(dirname "$(readlink -f "$0")")
readonly PROJECT_FULL_PATH=$(dirname "$SCRIPT_FULL_PATH")

PROTOCOL=""
DEVICES=""
STORE_URL=""
NIC=""
LOG_FILE="${BM_TEST_LOG:-/tmp/bm_test_$(date +%Y%m%d_%H%M%S).log}"

show_usage() {
    echo "Usage: $0 [options]"
    echo "  --protocol <name>      覆盖协议检测，透传 --bm-protocol"
    echo "  --devices <N0> <N1>    覆盖自动选卡，透传 --bm-devices（逻辑卡序）"
    echo "  --store-url <url>      透传 --bm-store-url"
    echo "  --nic <addr>           透传 --bm-nic"
    echo "  --log <file>           日志文件，默认 /tmp/bm_test_<时间戳>.log"
    echo "  -h, --help             显示帮助"
    exit 0
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --protocol) PROTOCOL="$2"; shift 2 ;;
        --devices) DEVICES="$2 $3"; shift 3 ;;
        --store-url) STORE_URL="$2"; shift 2 ;;
        --nic) NIC="$2"; shift 2 ;;
        --log) LOG_FILE="$2"; shift 2 ;;
        -h|--help) show_usage ;;
        *) echo "Unknown option: $1"; show_usage ;;
    esac
done

if ! command -v python3 >/dev/null 2>&1; then
    echo "ERROR: python3 not found" >&2
    exit 1
fi
python3 -m pytest --version >/dev/null 2>&1 || {
    echo "ERROR: pytest not installed; install it first (e.g. pip install pytest)" >&2
    exit 1
}
python3 -c "import torch_npu" >/dev/null 2>&1 || {
    echo "ERROR: torch_npu not importable; this test needs CANN/torch/torch_npu" >&2
    exit 1
}

mkdir -p "$(dirname "$LOG_FILE")"
cd "$PROJECT_FULL_PATH" || exit 1

ARGS=()
[[ -n "$PROTOCOL" ]] && ARGS+=(--bm-protocol "$PROTOCOL")
[[ -n "$DEVICES" ]] && ARGS+=(--bm-devices $DEVICES)
[[ -n "$STORE_URL" ]] && ARGS+=(--bm-store-url "$STORE_URL")
[[ -n "$NIC" ]] && ARGS+=(--bm-nic "$NIC")

echo "== pytest for BM ${ARGS[*]}"
echo "== log: $LOG_FILE"
set -o pipefail

python3 -m pytest "$PROJECT_FULL_PATH/test/integration" -v --log-cli-level=INFO "${ARGS[@]}" 2>&1 | tee "$LOG_FILE"
