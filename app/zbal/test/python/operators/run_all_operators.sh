#!/bin/bash
set -e

CURRENT_DIR=$(
    cd $(dirname ${BASH_SOURCE:-$0})
    pwd
)

cd $CURRENT_DIR

if [ "$1" = "-h" ] || [ "$1" = "--help" ] || [ "$1" = "help" ]; then
    cat << 'HELP'
Usage: bash run_all_operators.sh <world_sizes> <mode> [data_op_type] [mock]

Arguments:
  world_sizes   Comma-separated rank sizes, e.g. "4,8,16" (default: 4)
  mode          Test mode: smoke | precision | perf (default: smoke)
  data_op_type  Data operation type: 0=MTE, 1=AIV_SDMA, 2=AICPU_SDMA (default: 0)
  mock          Set to "mock" for fast dry-run without NPU hardware (default: disabled)

Examples:
  bash run_all_operators.sh 4 smoke                    # MTE, single rank=4
  bash run_all_operators.sh 4,8,16 perf 1              # SDMA, multi-rank perf
  bash run_all_operators.sh 4,8 precision 0 mock       # MTE mock dry-run
  bash run_all_operators.sh -h                         # Show this help
HELP
    exit 0
fi

WORLD_SIZES=${1:-4}
RUN_MODE=${2:-smoke}
DATA_OP_TYPE=${3:-0}
MOCK=${4:-0}

if [ "$DATA_OP_TYPE" != "0" ] && [ "$DATA_OP_TYPE" != "1" ] && [ "$DATA_OP_TYPE" != "2" ]; then
    echo "DATA_OP_TYPE must be 0 (MTE) or 1 (AIV_SDMA) or 2 (AICPU_SDMA), got: $DATA_OP_TYPE"
    exit 1
fi

# Format: "op_name:case_list"
SMOKE_CASE_CONFIG=(
    "allgather:     524288"
    "allreduce:     5242880"
    "alltoallv:     524288"
    "broadcast:     524289"
    "gather:        524288"
    "p2p:           5242880"
    "reducescatter: 5242880"
    "scatter:       5242880"
)

PRECISION_CASE_CONFIG=(
    "allgather:     524288,  5242880"
    "allreduce:     3,       5242880, 5242881"
    "alltoallv:     16,      5242880"
    "broadcast:     524289,  2097152, 16777216"
    "gather:        5242880"
    "p2p:           5242880"
    "reducescatter: 5242880"
    "scatter:       5242880"
)

# Perf test params (element counts): 16K 512K 4M 64M 128M
# bfloat16 operators: bytes/2, float operators(allreduce/reducescatter): bytes/4
PERF_CASE_CONFIG=(
    "allgather:     8192,    262144,  2097152, 33554432, 67108864"
    "allreduce:     4096,    131072,  1048576, 16777216, 33554432"
    "alltoallv:     8192,    262144,  2097152, 33554432, 67108864"
    "broadcast:     8192,    262144,  2097152, 33554432, 67108864"
    "gather:        8192,    262144,  2097152, 33554432, 67108864"
    "p2p:           8192,    262144,  2097152, 33554432, 67108864"
    "reducescatter: 4096,    131072,  1048576, 16777216, 33554432"
    "scatter:       8192,    262144,  2097152, 33554432, 67108864"
)

_collect_backend() {
    local op_name="$1" case="$2" backend="$3"
    local label="zbal"
    [ "$backend" = "h" ] && label="hccl"
    local found=0
    local pattern="$CURRENT_DIR/$op_name/profiling.${label}_*_${case}"
    for prof_dir in $pattern; do
        [ -d "$prof_dir" ] || continue
        local csv_file=$(find "$prof_dir" -name "kernel_details.csv" -print -quit 2>/dev/null)
        if [ -n "$csv_file" ]; then
            echo "  [collect] $op_name $label <- $csv_file"
            python3 "$CURRENT_DIR/perf_analyze.py" collect "$op_name" "$csv_file"
            found=1
        fi
    done
    if [ "$found" = "0" ]; then
        echo "  [collect] $op_name: no kernel_details.csv found in profiling.${label}_*_${case}/"
    fi
}

collect_perf_data() {
    local op_name="$1" case="$2"
    _collect_backend "$op_name" "$case" z
    _collect_backend "$op_name" "$case" h
    python3 "$CURRENT_DIR/perf_analyze.py" collect_final "$op_name" "$case"
}

# Track test results for pass/fail report (smoke/precision modes)
ALL_OPS=(allgather allreduce alltoallv broadcast gather p2p reducescatter scatter)
declare -A TEST_RESULTS_ALL

write_combined_pass_fail_report() {
    local ws_list=("$@")

    # Compute max operator name width
    local max_len=0
    for op in "${ALL_OPS[@]}"; do
        [ ${#op} -gt $max_len ] && max_len=${#op}
    done

    # Compute column widths
    local col_widths=()
    for ws in "${ws_list[@]}"; do
        col_widths+=($(( ${#ws} + 12 )))  # "rank=N" + padding
    done

    # Build header: Operator | rank=N | rank=M ...
    local header="| $(printf "%-${max_len}s" "Operator")"
    local i=0
    for ws in "${ws_list[@]}"; do
        header+=" | $(printf "%-${col_widths[$i]}s" "rank=${ws}")"
        i=$((i+1))
    done
    header+=" |"

    # Build separator
    local sep="| $(printf '%*s' $max_len '' | tr ' ' '-')"
    for w in "${col_widths[@]}"; do
        sep+=" | $(printf '%*s' $w '' | tr ' ' '-')"
    done
    sep+=" |"

    {
        echo ""
        printf "%s\n" "$header"
        printf "%s\n" "$sep"
        for op in "${ALL_OPS[@]}"; do
            local row="| $(printf "%-${max_len}s" "$op")"
            local i=0
            for ws in "${ws_list[@]}"; do
                local key="${op}_${ws}"
                local result="${TEST_RESULTS_ALL[$key]:-SKIP}"
                row+=" | $(printf "%-${col_widths[$i]}s" "$result")"
                i=$((i+1))
            done
            row+=" |"
            printf "%s\n" "$row"
        done
        echo ""
    } >> "$REPORT_FILE"
}

mock_test() {
    local op_name="$1"
    local case="$2"
    local ws="$3"
    local zbal_dir="$CURRENT_DIR/$op_name/profiling.zbal_${ws}_${case}"
    local hccl_dir="$CURRENT_DIR/$op_name/profiling.hccl_${ws}_${case}"

    declare -A KW=(
        [allgather_z]="ZBALAllGatherInner"    [allgather_h]="hcom_allGather__"
        [allreduce_z]="ZBALAllReduceInner"     [allreduce_h]="hcom_allReduce__"
        [alltoallv_z]="ZBALAlltoAllVInner"     [alltoallv_h]="hcom_alltoallv__"
        [broadcast_z]="ZBALBroadcastInner"    [broadcast_h]="hcom_broad"
        [gather_z]="ZBALBroadcastInner"        [gather_h]="hcom_broadcast__"
        [p2p_z]="ZBALSendInner"        [p2p_z2]="ZBALRecvInner"  [p2p_h]="hcom_send__"
        [reducescatter_z]="ZBALReduceScatterInner" [reducescatter_h]="hcom_reduceScatter__"
        [scatter_z]="ZBALScatterInner"        [scatter_h]="hcom_scatter"
    )

    local fake_time=$(( RANDOM % 100 + 10 ))
    mkdir -p "$zbal_dir" "$hccl_dir"
    for backend in z h; do
        local dir="$zbal_dir"
        [ "$backend" = "h" ] && dir="$hccl_dir"
        local kw="${KW[${op_name}_${backend}]}"
        echo "Step Id,Device_id,Model ID,Task ID,Stream ID,Name,Type,OP State,Accelerator Core,Start Time(us),Duration(us),Wait Time(us)" > "$dir/kernel_details.csv"
        echo "1,8,0,30,46,${kw},OP,dynamic,AI_VECTOR_CORE,1000,${fake_time}.00,0" >> "$dir/kernel_details.csv"
        echo "2,8,0,31,46,${kw},OP,dynamic,AI_VECTOR_CORE,1000,$((fake_time+5)).00,0" >> "$dir/kernel_details.csv"
        # p2p zbal has both send and recv kernels
        if [ "$op_name" = "p2p" ] && [ "$backend" = "z" ]; then
            echo "3,8,0,32,46,${KW[p2p_z2]},OP,dynamic,AI_VECTOR_CORE,1000,$((fake_time+2)).00,0" >> "$dir/kernel_details.csv"
        fi
    done
    echo "  [mock] $op_name profiling dirs created"
}

run_cases() {
    local config_name="${1:-PRECISION_CASE_CONFIG}"
    local -n config=$config_name

    for entry in "${config[@]}"; do
        op_name="${entry%%:*}"
        CASE_LIST="${entry#*:}"
        CASE_LIST=$(echo "$CASE_LIST" | tr -d ' ' | tr ',' ' ')
        subdir_path="$CURRENT_DIR/$op_name"

        local suite_mode="smoke"
        [ "${RUN_MODE}" = "perf" ] && suite_mode="perf"

        if [ "$MOCK" = "mock" ]; then
            for case in $CASE_LIST; do
                mock_test "$op_name" "$case" "${WORLD_SIZE}"
            done
            test_rc=0
        else
            (cd "$subdir_path" && bash test_zbal_$op_name.sh "$CASE_LIST" ${WORLD_SIZE} ${DATA_OP_TYPE} $suite_mode)
            test_rc=$?
        fi

        if [ $test_rc -eq 0 ]; then
            for case in $CASE_LIST; do
                collect_perf_data "$op_name" "$case"
            done
            TEST_RESULTS_ALL["${op_name}_${WORLD_SIZE}"]="PASS"
            echo "  [PASS] $op_name cases=$CASE_LIST"
        else
            TEST_RESULTS_ALL["${op_name}_${WORLD_SIZE}"]="FAIL"
            echo "  [FAIL] $op_name cases=$CASE_LIST"
            failed=1
        fi
    done

    if [ "${failed:-0}" = "1" ]; then
        echo ""
        echo "========================================="
        echo "  SOME TESTS FAILED"
        echo "========================================="
    else
        echo ""
        echo "========================================="
        echo "  ALL TESTS PASSED"
        echo "========================================="
    fi
}

# Parse world sizes (comma-separated, default 4)
IFS=',' read -ra WS_ARRAY <<< "${WORLD_SIZES}"

echo "=== ZBAL Test: mode=${RUN_MODE} world_sizes=${WORLD_SIZES} ==="

show_partial_perf() {
    local exit_code=$?
    # Only print on failure (non-zero exit)
    [ $exit_code -eq 0 ] && return
    # Print partial results from current (possibly failed) world_size
    if [ -f "$CURRENT_DIR/test_output/results.json" ]; then
        echo ""
        echo "=== Partial results (current world_size, collected before failure) ==="
        python3 "$CURRENT_DIR/perf_analyze.py" show 2>/dev/null || true
    fi
    # Print completed world_size results from report file
    if [ -f "$REPORT_FILE" ] && [ -s "$REPORT_FILE" ]; then
        echo ""
        echo "=== Completed world_size results (from $REPORT_FILE) ==="
        cat "$REPORT_FILE"
    fi
}
trap show_partial_perf EXIT

# Map data_op_type to name suffix
if [ "$DATA_OP_TYPE" = "0" ]; then
    OP_TYPE_NAME="MTE"
else
    OP_TYPE_NAME="SDMA"
fi

RESULT_NAME="perf_full_result_${OP_TYPE_NAME}.md"

rm -rf test_output
mkdir -p test_output

REPORT_FILE="$CURRENT_DIR/test_output/$RESULT_NAME"

# Write report header once
if [ "${RUN_MODE}" = "perf" ]; then
    echo "# ZBAL Performance Report" > "$REPORT_FILE"
else
    echo "# ZBAL Test Report" > "$REPORT_FILE"
fi
echo "" >> "$REPORT_FILE"

# Clean all profiling directories before starting
for op in "${ALL_OPS[@]}"; do
    rm -rf "$CURRENT_DIR/$op"/profiling.* 2>/dev/null
done

for WORLD_SIZE in "${WS_ARRAY[@]}"; do
    # Reset per-world_size state
    failed=0
    rm -f "$CURRENT_DIR/test_output/results.json"

    echo ""
    echo "========== world_size=${WORLD_SIZE} =========="

    case "${RUN_MODE}" in
        smoke)
            run_cases SMOKE_CASE_CONFIG
            ;;
        perf)
            export ZBAL_ENABLE_PERF_TEST=1
            run_cases PERF_CASE_CONFIG
            python3 "$CURRENT_DIR/perf_analyze.py" report ${WORLD_SIZE} >> "$REPORT_FILE" || true
            ;;
        precision)
            run_cases PRECISION_CASE_CONFIG
            ;;
    esac
done

# Write combined pass/fail table for smoke/precision
if [ "${RUN_MODE}" = "smoke" ] || [ "${RUN_MODE}" = "precision" ]; then
    write_combined_pass_fail_report "${WS_ARRAY[@]}"
fi

echo ""
echo "=== Final Report ==="
cat "$REPORT_FILE"

# Copy report to script directory
cp "$REPORT_FILE" "$CURRENT_DIR/$RESULT_NAME"
echo "Report copied to $CURRENT_DIR/$RESULT_NAME"

# Clean up mock profiling dirs
if [ "$MOCK" = "mock" ]; then
    for op in "${ALL_OPS[@]}"; do
        rm -rf "$CURRENT_DIR/$op"/profiling.zbal_* "$CURRENT_DIR/$op"/profiling.hccl_* 2>/dev/null
    done
    echo "[mock] profiling dirs cleaned"
fi

if [ "${failed:-0}" = "1" ]; then
    exit 1
fi