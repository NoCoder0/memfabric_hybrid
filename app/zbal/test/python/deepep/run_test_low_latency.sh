#!/bin/bash
set -e

usage() {
    echo "Usage: $0 [NP] [TOKENS] [HIDDEN] [TOPK] [EXPERTS]"
    echo ""
    echo "Positional arguments (all optional, with defaults):"
    echo "  NP        number of processes  (default: 8)"
    echo "  TOKENS    number of tokens     (default: 8)"
    echo "  HIDDEN    hidden dimension     (default: 7168)"
    echo "  TOPK      number of top-k      (default: 8)"
    echo "  EXPERTS   number of experts    (default: 256)"
    echo ""
    echo "Examples:"
    echo "  bash run_test_low_latency.sh"
    echo "  bash run_test_low_latency.sh 8"
    echo "  bash run_test_low_latency.sh 8 8 7168 8 256"
}

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    usage
    exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../../.." && pwd)"

get_installed_mf_commit() {
    local pkg_location
    pkg_location=$(pip show memfabric-hybrid 2>/dev/null \
        | grep '^Location:' \
        | awk '{print $2}')
    [ -z "${pkg_location}" ] && return 1

    strings "${pkg_location}"/memfabric_hybrid/_py* 2>/dev/null \
        | grep -w commit \
        | awk -F: '{print $NF}' \
        | head -1 \
        | tr -d '[:space:]' || true
}

# Build the memfabric_hybrid wheel from source and install it into the active env.
install_memfabric_hybrid() {
    local installed_commit current_commit
    installed_commit=$(get_installed_mf_commit || true)
    current_commit=$(cd "${REPO_ROOT}" && git rev-parse --short HEAD || echo unknown)

    cd "${REPO_ROOT}"

    if [ -n "${installed_commit}" ] && [[ "${installed_commit}" == "${current_commit}"* ]]; then
        echo "[run_test_low_latency] Installed memfabric_hybrid commit (${installed_commit}, HEAD=${current_commit}) matches, skipping rebuild."
    else
        echo "[run_test_low_latency] Commit mismatch (installed=${installed_commit:-<none>}, HEAD=${current_commit}), rebuilding ..."
        bash script/build.sh

        echo "[run_test_low_latency] uninstall memfabric_hybrid ..."
        pip uninstall memfabric_hybrid -y

        echo "[run_test_low_latency] Installing wheel ..."
        pip install output/memfabric_hybrid/wheel/memfabric_hybrid*
    fi

    # Restore cwd so the rest of the script's relative paths (./logs, test_low_latency.py) work.
    cd "${SCRIPT_DIR}"
}

check_memfabric_zbal_commit() {
    local installed_commit current_commit RED YEL RST pkg_location

    pkg_location=$(pip show memfabric_zbal 2>/dev/null \
        | grep '^Location:' \
        | awk '{print $2}')
    if [ -n "${pkg_location}" ]; then
        installed_commit=$(strings "${pkg_location}"/zbal/zbal* 2>/dev/null \
            | grep -w commit \
            | awk -F: '{print $NF}' \
            | head -1 \
            | tr -d '[:space:]' || true)
    fi

    [ -z "${installed_commit}" ] && return 0

    current_commit=$(cd "${REPO_ROOT}" && git rev-parse --short HEAD || echo unknown)

    if [[ "${installed_commit}" == "${current_commit}"* ]]; then
        echo "[run_test_low_latency] memfabric_zbal commit (${installed_commit}, HEAD=${current_commit}) matches."
        return 0
    fi

    RED='' YEL='' RST=''
    if [ -t 1 ] && [ -t 2 ]; then
        RED=$'\e[1;31m'
        YEL=$'\e[1;33m'
        RST=$'\e[0m'
    fi

    {
        printf '%s\n' "${RED}================================================================================"
        printf '%s\n' "${RED}  [run_test_low_latency] WARNING: memfabric_zbal COMMIT INDICATES NOT LATEST${RST}"
        printf '%s\n' "${RED}================================================================================${RST}"
        printf '%s\n' "  installed wheel commit : ${installed_commit}"
        printf '%s\n' "  current repo HEAD      : ${current_commit}"
        printf '%s\n' "${RED}================================================================================${RST}"
    } >&2
}

run_test_pass() {
    local rc
    set +e
    python "$@"
    rc=$?
    set -e
    if [ "${rc}" -ne 0 ]; then
        echo "[run_test_low_latency] WARNING: test pass exited with code ${rc}; continuing." >&2
    fi
    return "${rc}"
}

install_memfabric_hybrid

NP=${1:-8}
TOKENS=${2:-8}
HIDDEN=${3:-7168}
TOPK=${4:-8}
EXPERTS=${5:-256}

export TASK_QUEUE_ENABLE=2
export ASCEND_GLOBAL_LOG_LEVEL=2
rm -rf ./export_only_prof_dir/*

check_memfabric_zbal_commit

overall_rc=0

echo "[run_test_low_latency] Running test_low_latency.py with INT8 quantization"
export DEEP_LOW_LATENCY_MODE_USE_INT8_QUANT=1
run_test_pass test_low_latency.py --num-processes ${NP} --num-tokens ${TOKENS} --hidden ${HIDDEN} --num-topk ${TOPK} --num-experts ${EXPERTS} \
    || overall_rc=$?

echo "[run_test_low_latency] Running test_low_latency.py with BF16 (no quantization)"
export DEEP_LOW_LATENCY_MODE_USE_INT8_QUANT=0
run_test_pass test_low_latency.py --num-processes ${NP} --num-tokens ${TOKENS} --hidden ${HIDDEN} --num-topk ${TOPK} --num-experts ${EXPERTS} \
    || overall_rc=$?

exit "${overall_rc}"
