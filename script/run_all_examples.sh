#!/bin/bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# Licensed under Mulan PSL v2.
#
# Run ALL runnable examples in sequence.
#
# Usage:
#   bash script/run_all_examples.sh [options]
#
# Mode (default: run both Python and C++):
#  --python              Only run Python examples
#  --cpp                 Only run C++ examples (auto-builds run package if needed)
#   (no mode flag)        Run both Python and C++
#
# Options:
#  --store-url URL       Config store URL (default: tcp://127.0.0.1:8570)
#  --store-url-2 URL     Config store URL for 02_scale_out examples (default: tcp://127.0.0.1:8572)
#  --run-transfer        Also run transfer examples (needs 2 nodes)
#  --run-multi-node      Also run multi-node examples (needs 2 nodes)
#  --start-store         Automatically start config store before running (auto-stopped on exit)
#  --continue-on-error   Keep going even if an example fails (default: on)
#  --example-timeout N   Kill hanging examples after N seconds (default: 180)
#  --dry-run             Print what would be run without executing
#  --verbose             Print example stdout/stderr (default: hide, show only pass/fail)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
EXAMPLES_DIR="$PROJECT_DIR/examples"

# -- Debug + cleanup trap ------------------------------------------------------
DEBUG_LINE="start"
STORE_PID=""

cleanup_store() {
    if [[ -n "$STORE_PID" ]] && kill -0 "$STORE_PID" 2>/dev/null; then
        echo -e "  \e[36m[INFO]\e[0m  Stopping config store (PID $STORE_PID)..."
        kill "$STORE_PID" 2>/dev/null || true
    fi
}

on_exit() {
    local code=$?
    if [[ $code -ne 0 && $code -ne 130 ]]; then
        echo "[DEBUG] Exited at line: $DEBUG_LINE (code=$code)" >&2
    fi
    cleanup_store
    # Clean up residual C++ test processes that run.sh may leave behind
    for proc in shm_example AllReduce RDMADemo ShiftPutGet smem_bm_example; do
        if pgrep -x "$proc" &>/dev/null; then
            echo -e "  \e[33m[INFO]\e[0m  Cleaning up residual process: $proc"
            pkill -x "$proc" 2>/dev/null || true
        fi
    done
}
trap on_exit EXIT
trace() { DEBUG_LINE="$*"; }

# -- Defaults ------------------------------------------------------------------
STORE_URL="tcp://127.0.0.1:8570"
STORE_URL_2="tcp://127.0.0.1:8572"
EXAMPLE_TIMEOUT=180
RUN_PYTHON=true
RUN_CPP=true
RUN_TRANSFER=false
RUN_MULTI_NODE=false
START_STORE=false
CONTINUE_ON_ERROR=true
DRY_RUN=false
VERBOSE=false

# C++ build constants
SOC_VERSION="Ascend910B3"
SOC_ENV="A3"

# -- Parse args ----------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
  --python)          RUN_PYTHON=true;  RUN_CPP=false; shift ;;
  --cpp)             RUN_PYTHON=false; RUN_CPP=true;  shift ;;
  --build-cpp)       RUN_CPP=true;                     shift ;;
  --store-url)       STORE_URL="$2";       shift 2 ;;
  --store-url-2)     STORE_URL_2="$2";     shift 2 ;;
  --run-transfer)    RUN_TRANSFER=true;    shift ;;
  --run-multi-node)  RUN_MULTI_NODE=true;  shift ;;
  --start-store)     START_STORE=true;     shift ;;
  --continue-on-error) CONTINUE_ON_ERROR=true; shift ;;
  --example-timeout) EXAMPLE_TIMEOUT="$2";  shift 2 ;;
  --dry-run)         DRY_RUN=true;         shift ;;
  --verbose)         VERBOSE=true;         shift ;;
  --help)
            head -28 "$0" | sed -n '/^#/p' | sed 's/^#//'
            exit 0
            ;;
        *)
            echo "[ERROR] Unknown option: $1"
            exit 1
            ;;
    esac
done

# -- Helpers -------------------------------------------------------------------
PASS=0
FAIL=0
SKIP=0
FAILED_NAMES=()

info()  { echo -e "  \e[36m[INFO]\e[0m  $*"; }
pass()  { echo -e "  \e[32m[PASS]\e[0m  $*"; PASS=$((PASS + 1)); }
fail()  { echo -e "  \e[31m[FAIL]\e[0m  $*"; FAIL=$((FAIL + 1)); FAILED_NAMES+=("$1"); }
skip()  { echo -e "  \e[33m[SKIP]\e[0m  $*"; SKIP=$((SKIP + 1)); }
header(){ echo -e "\n\e[1;34m=== $* ===\e[0m"; }
sub()   { echo -e "  \e[90m$ $*\e[0m"; }

# Check that specific device IDs are idle
devices_idle() {
    local ids="${NPU_IDLE_IDS:-,}"
    local d
    for d in "$@"; do
        if [[ ",$ids," != *",$d,"* ]]; then
            return 1
        fi
    done
    return 0
}

run_example() {
    local name="$1"
    local dir="$2"
    local cmd="$3"
    shift 3

    if $DRY_RUN; then
        echo -e "  \e[90m[DRY-RUN] cd $dir && $cmd\e[0m"
        return
    fi

    trace "cd $dir"
    cd "$dir"

    local timeout_cmd
    if command -v timeout &>/dev/null; then
        timeout_cmd="timeout $EXAMPLE_TIMEOUT"
    else
        timeout_cmd=""
    fi

    local output
    local ret=0
    if $VERBOSE; then
        echo ""
        sub "$cmd"
        bash -c "$timeout_cmd $cmd" || ret=$?
    else
        output=$(bash -c "$timeout_cmd $cmd" 2>&1) || ret=$?
    fi

    if [[ $ret -eq 0 ]]; then
        pass "$name"
    else
        if [[ $ret -eq 124 ]]; then
            fail "$name (TIMEOUT after ${EXAMPLE_TIMEOUT}s)"
        else
            # Retry once on first failure (workaround for cold-start device init races)
            info "Retrying $name ..."
            local ret2=0
            if $VERBOSE; then
                bash -c "$timeout_cmd $cmd" || ret2=$?
            else
                output=$(bash -c "$timeout_cmd $cmd" 2>&1) || ret2=$?
            fi
            if [[ $ret2 -eq 0 ]]; then
                pass "$name (retry)"
                trace "cd $PROJECT_DIR"
                cd "$PROJECT_DIR"
                return
            fi
            fail "$name"
        fi
        if ! $VERBOSE && [[ -n "${output:-}" ]]; then
            echo -e "  \e[90m$(echo "$output" | tail -5 | sed 's/^/  | /')\e[0m"
        fi
        if ! $CONTINUE_ON_ERROR; then
            echo -e "  \e[31mStopping due to error (use --continue-on-error to skip).\e[0m"
            exit 1
        fi
    fi
    trace "cd $PROJECT_DIR"
    cd "$PROJECT_DIR"
}

run_python_example() {
    local name="$1"
    local dir="$2"
    shift 2
    run_example "$name" "$dir" "python3 $(basename "$dir").py $*"
}

run_python_example_args() {
    local name="$1"
    local dir="$2"
    local args="$3"
    run_example "$name" "$dir" "python3 $(basename "$dir").py $args"
}

# -- Ensure C++ deps: detect -> build -> install -> source env -------------------
cpp_deps_ready=false

find_set_env() {
    for p in \
        "/usr/local/memfabric_hybrid/set_env.sh" \
        "$HOME/memfabric_hybrid/set_env.sh"
    do
        [[ -f "$p" ]] && { echo "$p"; return 0; }
    done
    return 1
}

find_run_pkg() {
    ls -1 "$PROJECT_DIR"/output/memfabric_hybrid-*.run 2>/dev/null | head -1
}

build_run_pkg() {
    local run_pkg
    run_pkg=$(find_run_pkg)
    if [[ -z "$run_pkg" ]]; then
        info "Run package not found, building from source (this may take a while)..."
        if ! bash "$SCRIPT_DIR/build_and_pack_run.sh"; then
            echo -e "  \e[31mBuild failed. Check errors above.\e[0m"
            return 1
        fi
        run_pkg=$(find_run_pkg)
    fi
    echo "$run_pkg"
    return 0
}

ensure_python_deps() {
    if python3 -c "import memfabric_hybrid" 2>/dev/null; then
        return 0
    fi
    info "memfabric_hybrid Python package not found, building..."
    local run_pkg
    run_pkg=$(build_run_pkg) || return 1
    if [[ -n "$run_pkg" ]]; then
        info "Installing run package: $(basename "$run_pkg")"
        bash "$run_pkg" --no-check || return 1
        if python3 -c "import memfabric_hybrid" 2>/dev/null; then
            return 0
        fi
    fi
    return 1
}

ensure_cpp_deps() {
    # 1. Already set and valid?
    if [[ -n "${MEMFABRIC_HYBRID_HOME_PATH:-}" ]] && \
       [[ -d "$MEMFABRIC_HYBRID_HOME_PATH/include/smem" ]]; then
        info "MEMFABRIC_HYBRID_HOME_PATH already set: $MEMFABRIC_HYBRID_HOME_PATH"
        cpp_deps_ready=true
        return 0
    fi

    # 2. Installed at default location?
    local set_env
    if set_env=$(find_set_env); then
        info "Found installed run package, sourcing: $set_env"
        source "$set_env"
        cpp_deps_ready=true
        return 0
    fi

    # 3. Build -> install
    info "Run package not found, building from source (this may take a while)..."
    local run_pkg
    run_pkg=$(build_run_pkg) || return 1

    if [[ -n "$run_pkg" ]]; then
        info "Installing run package: $(basename "$run_pkg")"
        bash "$run_pkg" --no-check || return 1
        if set_env=$(find_set_env); then
            source "$set_env"
            cpp_deps_ready=true
            return 0
        fi
    fi

    return 1
}

detect_npu_info() {
    # total idle idle_ids
    local total=0 idle=0 idle_ids=""
    total=$(ls -d /dev/davinci[0-9]* 2>/dev/null | wc -l || true)
    if [[ "$total" -eq 0 ]]; then
        echo "0 0"
        return
    fi
    if command -v npu-smi &>/dev/null; then
        local output
        output=$(npu-smi info 2>/dev/null) || { echo "$total 0"; return; }
        #  NPU ID "No running processes found in NPU X"  X
        if grep -P "" /dev/null 2>/dev/null; then
            idle_ids=$(echo "$output" | grep -oP "No running processes found in NPU \K[0-9]+" | tr '\n' ',' | sed 's/,$//' || true)
        else
            idle_ids=$(echo "$output" | sed -n 's/.*No running processes found in NPU \([0-9][0-9]*\).*/\1/p' | paste -sd ',' || true)
        fi
        if [[ -n "$idle_ids" ]]; then
            idle=$(echo "$idle_ids" | tr ',' '\n' | wc -l)
        fi
    fi
    echo "$total $idle $idle_ids"
}

# Check whether DRAM SDMA is supported on this environment
# A2 (1 chip) uses ConnBasedSegment -> SDMA unsupported on DRAM.
# A3 (2 chips) uses VmmBasedSegment -> SDMA supported.
detect_dram_sdma_support() {
    if ! command -v npu-smi &>/dev/null; then
        return 1
    fi
    local chip_count
    chip_count=$(npu-smi info -t board -i 0 2>/dev/null | grep "Chip Count" | grep -oP '\d+' || true)
    if [[ "$chip_count" -ge 2 ]]; then
        return 0
    fi
    return 1
}

# -- Detect environment --------------------------------------------------------
echo -e "\e[1mMemFabric Hybrid  Run All Examples\e[0m"
echo -e "  Project: $PROJECT_DIR"
echo -e "  Store URL: $STORE_URL"
if $RUN_PYTHON && $RUN_CPP; then
    echo -e "  Mode: all (Python + C++)"
elif $RUN_PYTHON; then
    echo -e "  Mode: Python only"
else
    echo -e "  Mode: C++ only"
fi
echo ""

NPU_TOTAL=0
NPU_IDLE=0
NPU_IDLE_IDS=""
read -r NPU_TOTAL NPU_IDLE NPU_IDLE_IDS < <(detect_npu_info)
info "NPU: ${NPU_TOTAL} total, ${NPU_IDLE} idle (IDs: ${NPU_IDLE_IDS:-none})"

# -- Config store --------------------------------------------------------------
if $START_STORE; then
    info "Starting config store at $STORE_URL ..."
    export MF_CONFIG_STORE_URL="$STORE_URL"
    python3 -c "from memfabric_hybrid import create_config_store; create_config_store('$STORE_URL')" &
    STORE_PID=$!
    sleep 2
    info "Config store PID: $STORE_PID"
    echo ""
fi

# ------------------------------------------------------------------------------
#  Python examples
# ------------------------------------------------------------------------------

if $RUN_PYTHON && ! $DRY_RUN; then
    if ! ensure_python_deps; then
        echo -e "  \e[31m[ERROR] memfabric_hybrid Python package not available. Run 'bash script/build_and_pack_run.sh' or check pip installation.\e[0m"
        skip "All Python examples (Python package not available)"
        RUN_PYTHON=false
    fi
fi

DRAM_SDMA_SUPPORTED=true
if $RUN_PYTHON; then
    if detect_dram_sdma_support; then
        info "DRAM SDMA: supported"
    else
        info "DRAM SDMA: NOT supported (SDMA-only examples will be skipped)"
        DRAM_SDMA_SUPPORTED=false
    fi
fi

# ------------------------------------------------------------------------------
#  01_basic  single-device examples
# ------------------------------------------------------------------------------
header "01_basic  Single-Device Memory Pool"

if [[ $NPU_IDLE -ge 1 ]]; then
    for ex in \
        "01_single_device_dram_pool" \
        "02_single_device_dram_configurable_pool" \
        "03_single_device_hbm_pool"
    do
        d="$EXAMPLES_DIR/memory_pool/01_basic/$ex"
        if [[ -f "$d/$ex.py" ]]; then
            trace "about to run: $ex"
            run_python_example "$ex" "$d"
            trace "completed: $ex"
        else
            skip "$ex (no runnable script found)"
        fi
    done
    # 06_single_card_external_stream relies on SDMA
    if $DRAM_SDMA_SUPPORTED; then
        ex="06_single_card_external_stream"
        d="$EXAMPLES_DIR/memory_pool/01_basic/$ex"
        if [[ -f "$d/$ex.py" ]]; then
            trace "about to run: $ex"
            run_python_example "$ex" "$d"
            trace "completed: $ex"
        fi
    else
        skip "06_single_card_external_stream (SDMA not supported on this environment)"
    fi
else
    skip "01_basic/* (no NPU available, need >=1)"
fi

# 04/05 are README-only (manual adaptation of 01)
skip "04_no_xpu_host_rdma_dram_pool (README-only, manual adaptation)"
skip "05_no_xpu_host_urma_dram_pool (README-only, manual adaptation)"

# ------------------------------------------------------------------------------
#  02_scale_out  multi-device examples
# ------------------------------------------------------------------------------
header "02_scale_out  Multi-Device Memory Pool"

if devices_idle 0 1; then
    d="$EXAMPLES_DIR/memory_pool/02_scale_out/01_single_node_multi_device_dram"
    if [[ -f "$d/01_single_node_multi_device_dram.py" ]]; then
        # Override default store URL for this example
        run_python_example "01_single_node_multi_device_dram" "$d"
    fi
else
    skip "01_single_node_multi_device_dram (need device 0,1 idle, got: ${NPU_IDLE_IDS:-none})"
fi

if $RUN_MULTI_NODE; then
    d="$EXAMPLES_DIR/memory_pool/02_scale_out/02_multi_node_multi_device_dram"
    if [[ -f "$d/02_multi_node_multi_device_dram.py" ]]; then
        info "Multi-node example requires 2 machines. Run manually:"
        info "  # Node 0: python3 $d/02_multi_node_multi_device_dram.py 0 <head_ip>"
        info "  # Node 1: python3 $d/02_multi_node_multi_device_dram.py 1 <head_ip>"
        skip "02_multi_node_multi_device_dram (manual multi-node only)"
    fi
else
    skip "02_multi_node_multi_device_dram (use --run-multi-node to enable)"
fi

# ------------------------------------------------------------------------------
#  03_optimization  performance examples
# ------------------------------------------------------------------------------
header "03_optimization  Performance Optimization"

if [[ $NPU_IDLE -ge 1 ]]; then
    for ex in "01_copy_data_batch" "02_register"; do
        d="$EXAMPLES_DIR/memory_pool/03_optimization/$ex"
        if [[ -f "$d/$ex.py" ]]; then
            run_python_example "$ex" "$d"
        else
            skip "$ex (no runnable script found)"
        fi
    done
    # 03_device_sdma relies on SDMA
    if $DRAM_SDMA_SUPPORTED; then
        ex="03_device_sdma"
        d="$EXAMPLES_DIR/memory_pool/03_optimization/$ex"
        if [[ -f "$d/$ex.py" ]]; then
            run_python_example "$ex" "$d"
        fi
    else
        skip "03_device_sdma (SDMA not supported on this environment)"
    fi
else
    skip "03_optimization/* (need >=1 NPU)"
fi

# ------------------------------------------------------------------------------
#  04_features  feature examples
# ------------------------------------------------------------------------------
header "04_features  Feature Showcase"

skip "01_enable_unified_address_space (README-only design draft)"

if [[ $NPU_IDLE -ge 1 ]]; then
    local_ranks=$NPU_IDLE
    # extend_local_mem: --local_ranks=8, --world_size=8;  NPU
    # Defaults to device_sdma protocol; fall back to device_rdma if SDMA unsupported
    d="$EXAMPLES_DIR/memory_pool/04_features/02_extend_local_mem"
    if [[ -f "$d/02_extend_local_mem.py" ]]; then
        em_args="--local_ranks=$local_ranks --world_size=$local_ranks"
        if ! $DRAM_SDMA_SUPPORTED; then
            em_args+=" --protocol device_rdma"
        fi
        run_python_example_args "02_extend_local_mem" "$d" "$em_args"
    fi

    # enable_56bits_gva: --local_ranks=8, --world_size=1024
    # Defaults to device_sdma protocol; fall back to device_rdma if SDMA unsupported
    d="$EXAMPLES_DIR/memory_pool/04_features/03_enable_56bits_gva"
    if [[ -f "$d/03_enable_56bits_gva.py" ]]; then
        gva_args="--local_ranks=$local_ranks"
        if ! $DRAM_SDMA_SUPPORTED; then
            gva_args+=" --protocol device_rdma"
        fi
        run_python_example_args "03_enable_56bits_gva" "$d" "$gva_args"
    fi
else
    skip "04_features/* (need >=1 NPU)"
fi

# ------------------------------------------------------------------------------
#  05_observability  design drafts only
# ------------------------------------------------------------------------------
header "05_observability  Observability"

skip "01_prometheus_grafana (README-only design draft)"
skip "02_opentelemetry (README-only design draft)"
skip "03_dashboards (README-only design draft)"

# ------------------------------------------------------------------------------
#  transfer examples
# ------------------------------------------------------------------------------
header "Transfer Examples"

if $RUN_TRANSFER; then
    info "Transfer examples need a 2-node setup."
    info "  Node 1 (Decode): python3 $EXAMPLES_DIR/transfer/test_transfer_engine.py --role Decode --store-url $STORE_URL ..."
    info "  Node 2 (Prefill): python3 $EXAMPLES_DIR/transfer/test_transfer_engine.py --role Prefill --store-url $STORE_URL ..."
    info "  (and similarly for test_quant_trans.py)"
    skip "test_transfer_engine (manual 2-node)"
    skip "test_quant_trans (manual 2-node)"
else
    skip "transfer/* (use --run-transfer to enable)"
fi

# ------------------------------------------------------------------------------
#  hbm_share_memory  C++ examples
#  Re-detect NPU idle status (Python examples may have released their devices)
# ------------------------------------------------------------------------------
header "hbm_share_memory  C++ Examples (requires run package)"
read -r NPU_TOTAL NPU_IDLE NPU_IDLE_IDS < <(detect_npu_info)

if $RUN_CPP; then
    if $DRY_RUN; then
        cpp_deps_ready=true
    elif ! ensure_cpp_deps; then
        skip "hbm_share_memory/* (failed to prepare run package)"
        skip "AllReduce (run package not available)"
        skip "RDMADemo (run package not available)"
        skip "ShiftPutGet (run package not available)"
        cpp_deps_ready=false
    fi

    if $cpp_deps_ready; then
    for ex_dir in "AllReduce" "RDMADemo" "ShiftPutGet"; do
        d="$EXAMPLES_DIR/hbm_share_memory/$ex_dir"
        if [[ ! -f "$d/build.sh" ]]; then
            skip "$ex_dir (no build.sh)"
            continue
        fi
        # ShiftPutGet uses ENV version (A2/A3/A5), others use SOC version (Ascend910B3)
        build_ver="$SOC_VERSION"
        [[ "$ex_dir" == "ShiftPutGet" ]] && build_ver="$SOC_ENV"
        if $DRY_RUN; then
            echo -e "  \e[90m[DRY-RUN] cd $d && bash build.sh -v $build_ver\e[0m"
            echo -e "  \e[90m[DRY-RUN] cd $d && bash run.sh 2 $STORE_URL\e[0m"
            continue
        fi
        info "Building $ex_dir (soc_ver=$build_ver) ..."
        cd "$d"
        if ! bash build.sh -v "$build_ver" >/dev/null 2>&1; then
            fail "$ex_dir (build failed)"
            if ! $CONTINUE_ON_ERROR; then exit 1; fi
            cd "$PROJECT_DIR"
            continue
        fi
        if [[ $NPU_IDLE -lt 2 ]] || ! devices_idle 0 1; then
            skip "$ex_dir run (need NPU 0,1 idle, got: ${NPU_IDLE_IDS:-none})"
            cd "$PROJECT_DIR"
            continue
        fi
        if bash run.sh 2 "$STORE_URL" >/dev/null 2>&1; then
            pass "$ex_dir"
        else
            fail "$ex_dir (run failed)"
            if ! $CONTINUE_ON_ERROR; then exit 1; fi
        fi
        cd "$PROJECT_DIR"
    done
    fi
else
    skip "AllReduce (use --cpp to enable)"
    skip "RDMADemo (use --cpp to enable)"
    skip "ShiftPutGet (use --cpp to enable)"
fi

# ------------------------------------------------------------------------------
#  Summary
# ------------------------------------------------------------------------------
echo ""
echo -e "\e[1;34m=========================================\e[0m"
echo -e "  \e[1mSummary:\e[0m"
echo -e "  \e[32mPASS:  $PASS\e[0m"
echo -e "  \e[31mFAIL:  $FAIL\e[0m"
echo -e "  \e[33mSKIP:  $SKIP\e[0m"
if [[ ${#FAILED_NAMES[@]} -gt 0 ]]; then
    echo -e "  \e[31mFailed: ${FAILED_NAMES[*]}\e[0m"
fi
echo -e "\e[1;34m=========================================\e[0m"

if [[ $FAIL -gt 0 ]]; then
    exit 1
fi
