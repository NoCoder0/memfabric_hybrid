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
    echo "Runs one pass per quant type, sequentially, with the same parameters."
    echo "The chip generation (A3/A5) is auto-detected to pick the default cases:"
    echo "  A5: int8 none mx_fp8_e4m3"
    echo "  A3: int8 none"
    echo ""
    echo "Examples:"
    echo "  bash run_test_normal.sh"
    echo "  bash run_test_normal.sh 8"
    echo "  bash run_test_normal.sh 8 1024 7168 8 256"
    echo "  DEEP_NORMAL_MODE_QUANT_TYPE='int8 none mx_fp8_e4m3' bash run_test_normal.sh"
}

if [ "$1" = "-h" ] || [ "$1" = "--help" ]; then
    usage
    exit 0
fi

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../../../.." && pwd)"

A5_ONLY_QUANT_TYPES="mx_fp8_e4m3"

detect_chip_type() {
    local soc
    soc=$(python -c "
import acl
try:
    print(acl.get_soc_name() or '')
except Exception:
    pass" 2>/dev/null | tail -1)
    case "${soc}" in
        *Ascend950* | *Ascend910_95*) echo "A5" ;;
        *) echo "A3" ;;
    esac
}

is_a5_only_quant_type() {
    case " ${A5_ONLY_QUANT_TYPES} " in
        *" $1 "*) return 0 ;;
        *) return 1 ;;
    esac
}

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
        echo "[run_test_normal] Installed memfabric_hybrid commit (${installed_commit}, HEAD=${current_commit}) matches, skipping rebuild."
    else
        echo "[run_test_normal] Commit mismatch (installed=${installed_commit:-<none>}, HEAD=${current_commit}), rebuilding ..."
        bash script/build.sh

        echo "[run_test_normal] uninstall memfabric_hybrid ..."
        pip uninstall memfabric_hybrid -y

        echo "[run_test_normal] Installing wheel ..."
        pip install output/memfabric_hybrid/wheel/memfabric_hybrid*
    fi

    # Restore cwd so the rest of the script's relative paths (./logs, test_normal.py) work.
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
        echo "[run_test_normal] memfabric_zbal commit (${installed_commit}, HEAD=${current_commit}) matches."
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
        printf '%s\n' "${RED}  [run_test_normal] WARNING: memfabric_zbal COMMIT INDICATES NOT LATEST${RST}"
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
        echo "[run_test_normal] WARNING: test pass exited with code ${rc}; continuing." >&2
    fi
    return "${rc}"
}

# Collect the root-rank average bandwidth/latency lines emitted by every pass.
summarize_avg_stats() {
    local log_dir="${SCRIPT_DIR}/logs"
    local -a rows=()
    local f row qtype dbw dlat cbw clat

    for f in "${log_dir}"/rank00_pid*.log; do
        [ -f "${f}" ] || continue
        row=$(awk '
            /Average Dispatch bandwidth/ {
                found = 1
                if (match($0, /quant_type=[^)]*/)) qtype = substr($0, RSTART + 11, RLENGTH - 11)
                if (match($0, /: [0-9.]+ GB\/s/)) dbw = substr($0, RSTART + 2, RLENGTH - 7)
                if (match($0, /avg_t=[0-9.]+/)) dlat = substr($0, RSTART + 6, RLENGTH - 6)
            }
            /Average Combine bandwidth/ {
                found = 1
                if (match($0, /: [0-9.]+ GB\/s/)) cbw = substr($0, RSTART + 2, RLENGTH - 7)
                if (match($0, /avg_t=[0-9.]+/)) clat = substr($0, RSTART + 6, RLENGTH - 6)
            }
            END {
                if (found) {
                    if (qtype == "") qtype = "(unknown)"
                    if (dbw == "") dbw = "-"
                    if (dlat == "") dlat = "-"
                    if (cbw == "") cbw = "-"
                    if (clat == "") clat = "-"
                    print qtype, dbw, dlat, cbw, clat
                }
            }
        ' "${f}")
        [ -n "${row}" ] || continue
        rows+=("${row}")
    done

    echo ""
    echo "==================== Average Bandwidth Summary ===================="
    if [ "${#rows[@]}" -eq 0 ]; then
        echo "  (no average stats found; expected in ${log_dir}/rank00_pid*.log)"
    else
        printf '  %-20s %14s %12s %14s %12s\n' "quant_type" "Dispatch(GB/s)" "Dispatch(us)" "Combine(GB/s)" "Combine(us)"
        for row in "${rows[@]}"; do
            read -r qtype dbw dlat cbw clat <<< "${row}"
            printf '  %-20s %14s %12s %14s %12s\n' "${qtype}" "${dbw}" "${dlat}" "${cbw}" "${clat}"
        done
    fi
    echo "==================================================================="
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
rm -rf ./logs

check_memfabric_zbal_commit

CHIP_TYPE=$(detect_chip_type)
if [ "${CHIP_TYPE}" = "A5" ]; then
    DEFAULT_QUANT_TYPES="int8 none ${A5_ONLY_QUANT_TYPES}"
else
    DEFAULT_QUANT_TYPES="int8 none"
fi
QUANT_TYPES="${DEEP_NORMAL_MODE_QUANT_TYPE:-${DEFAULT_QUANT_TYPES}}"

echo "[run_test_normal] Detected chip type: ${CHIP_TYPE}, quant types to run: ${QUANT_TYPES}"

overall_rc=0

for qt in ${QUANT_TYPES}; do
    if [ "${CHIP_TYPE}" != "A5" ] && is_a5_only_quant_type "${qt}"; then
        echo "[run_test_normal] Skipping quant_type=${qt}: requires A5, detected ${CHIP_TYPE}" >&2
        continue
    fi
    echo "[run_test_normal] Running test_normal.py with quant_type=${qt}"
    export DEEP_NORMAL_MODE_QUANT_TYPE=${qt}
    run_test_pass test_normal.py --num-processes ${NP} --num-tokens ${TOKENS} --hidden ${HIDDEN} --num-topk ${TOPK} --num-experts ${EXPERTS} \
        || overall_rc=$?
done

# Extra pass covering the prefix-sum token-count format (default passes use MOE_EXPERT_TOKEN_NUMS_TYPE=1)
echo "[run_test_normal] Running test_normal.py with quant_type=none + prefix-sum token counts (MOE_EXPERT_TOKEN_NUMS_TYPE=0)"
export DEEP_NORMAL_MODE_QUANT_TYPE=none
export MOE_EXPERT_TOKEN_NUMS_TYPE=0
run_test_pass test_normal.py --num-processes ${NP} --num-tokens ${TOKENS} --hidden ${HIDDEN} --num-topk ${TOPK} --num-experts ${EXPERTS} \
    || overall_rc=$?
export MOE_EXPERT_TOKEN_NUMS_TYPE=1

summarize_avg_stats

exit "${overall_rc}"
