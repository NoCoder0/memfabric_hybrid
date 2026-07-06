#!/bin/bash
set -e

usage() {
    echo "Usage: $0 [NP] [TOKENS] [HIDDEN] [TOPK] [EXPERTS]"
    echo ""
    echo "Positional arguments (all optional, with defaults):"
    echo "  NP        number of processes  (default: 8)"
    echo "  TOKENS    number of tokens     (default: 1024)"
    echo "  HIDDEN    hidden dimension     (default: 7168)"
    echo "  TOPK      number of top-k      (default: 8)"
    echo "  EXPERTS   number of experts    (default: 256)"
    echo ""
    echo "Runs two passes:"
    echo "  Step 1 (quick verify): 2 processes, 64 tokens, hidden=1024"
    echo "  Step 2 (full test):    NP processes, full parameters"
    echo ""
    echo "Examples:"
    echo "  bash run_test_normal.sh"
    echo "  bash run_test_normal.sh 8"
    echo "  bash run_test_normal.sh 8 1024 7168 8 256"
}

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    usage
    exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../../.." && pwd)"

# Build the memfabric_hybrid wheel from source and install it into the active env.
install_memfabric_hybrid() {
    echo "[run_test_normal] Building wheel at ${REPO_ROOT} ..."
    cd "${REPO_ROOT}"
    bash script/build.sh

    echo "[run_test_normal] uninstall memfabric_hybrid ..."
    pip uninstall memfabric_hybrid -y

    echo "[run_test_normal] Installing wheel ..."
    pip install output/memfabric_hybrid/wheel/memfabric_hybrid*

    # Restore cwd so the rest of the script's relative paths (./logs, test_normal.py) work.
    cd "${SCRIPT_DIR}"
}

install_memfabric_hybrid

NP=${1:-8}
TOKENS=${2:-1024}
HIDDEN=${3:-7168}
TOPK=${4:-8}
EXPERTS=${5:-256}

export TASK_QUEUE_ENABLE=2
export ASCEND_PROCESS_LOG_PATH=./logs
export ASCEND_GLOBAL_LOG_LEVEL=2
export DEEP_NORMAL_MODE_USE_INT8_QUANT=1
rm -rf ./logs

python test_normal.py --num-processes ${NP} --num-tokens ${TOKENS} --hidden ${HIDDEN} --num-topk ${TOPK} --num-experts ${EXPERTS}