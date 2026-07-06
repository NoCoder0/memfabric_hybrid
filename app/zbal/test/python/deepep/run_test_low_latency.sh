#!/bin/bash
set -e

NP=${1:-8}
TOKENS=${2:-8}
HIDDEN=${3:-7168}
TOPK=${4:-8}
EXPERTS=${5:-128}

usage() {
    echo "Usage: $0 [NP] [TOKENS] [HIDDEN] [TOPK] [EXPERTS]"
    echo ""
    echo "Positional arguments (all optional, with defaults):"
    echo "  NP        number of processes  (default: 8)"
    echo "  TOKENS    number of tokens     (default: 8)"
    echo "  HIDDEN    hidden dimension     (default: 7168)"
    echo "  TOPK      number of top-k      (default: 8)"
    echo "  EXPERTS   number of experts    (default: 128)"
    echo ""
    echo "Examples:"
    echo "  bash run_test_low_latency.sh"
    echo "  bash run_test_low_latency.sh 8"
    echo "  bash run_test_low_latency.sh 8 8 7168 8 128"
}

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    usage
    exit 0
fi

export TASK_QUEUE_ENABLE=2
export ASCEND_PROCESS_LOG_PATH=./logs
export ASCEND_GLOBAL_LOG_LEVEL=2
rm -rf ./logs
rm -rf ./export_only_prof_dir/*

python test_low_latency.py --num-processes ${NP} --num-tokens ${TOKENS} --hidden ${HIDDEN} --num-topk ${TOPK} --num-experts ${EXPERTS}