#!/usr/bin/env bash
# Usage: run_stage_compare.sh mf|sgl local|remote baseline|trace RUN_ID -- baseline connection/CPU args
set -euo pipefail
app="${1:?app}"; role="${2:?role}"; mode="${3:?baseline or trace}"; run="${4:?run ID}"
shift 4
[[ "${1:-}" != -- ]] || shift
[[ "$app" == mf || "$app" == sgl ]] || exit 2
[[ "$role" == local || "$role" == remote ]] || exit 2
[[ "$mode" == baseline || "$mode" == trace ]] || exit 2
mf="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
parent="$(dirname -- "$mf")"
source "$mf/output/stage-build.env"
export HCOM_CAPTURE_IDENTITY=1
export LD_LIBRARY_PATH="${STAGE_HCOM_LIB}:${STAGE_BOUNDSCHECK_ROOT}/lib:${LD_LIBRARY_PATH:-}"
out="${STAGE_RESULT_DIR:-${parent}/stage-results}"
prefix="${out}/${run}-${app}-${role}"
capture=(python3 "$mf/test/indirect_transport_test/capture_stage_run.py" --app "$app" --role "$role"
    --comparison-id "$run" --run-id "${run}-${app}-${role}-${mode}"
    --hcom-repo "${MF_HCOM_SOURCE_DIR:-${parent}/ubs-comm}" --build-manifest "$mf/output/stage-build.json"
    --output "${prefix}-${mode}")
if [[ "$mode" == baseline ]]; then
    capture+=(--trace-off)
else
    capture+=(--baseline "${prefix}-baseline.compact.json")
fi
if [[ "$app" == mf ]]; then
    export MF_HCOM_SQ_SIZE=1024
    unset MF_BENCH_RAIL_TRACE
    # Preserve all other baseline environment settings, including submission/CPU/NIC settings.
    for lib in libmf_smem.so libmf_hybm_core.so; do
        mapfile -t paths < <(find "$mf/output" -name "$lib")
        [[ ${#paths[@]} == 1 ]] || { echo "ERROR: ambiguous $lib in output" >&2; exit 2; }
        export LD_LIBRARY_PATH="$(dirname -- "${paths[0]}"):${LD_LIBRARY_PATH}"
    done
    extra=(--trace=0 --rounds=1000)
    [[ "$mode" != trace ]] || extra=(--trace=1 --rounds=3)
    "${capture[@]}" --repo "$mf" -- "$mf/test/indirect_transport_test/hostrdma_batch_bench" "$@" \
        --role="$role" --mode=cont --count=1600 --size=656 --stride=4096 --chunk=960 --warmup=100 "${extra[@]}"
else
    sgl="${SGL_SOURCE_DIR:-${parent}/perf_test_duo_card_sgl}"
    export RDMA_600_SGL_ITEMS=30
    extra=(--kind measure --rounds 1000)
    [[ "$mode" != trace ]] || extra=(--kind trace --trace-rounds 3)
    "${capture[@]}" --repo "$sgl" -- "$sgl/build/rdma_600" "$@" --role "$role" --links 1 --mode sgl \
        --pipeline on --blocks 1600 --block-bytes 656 --notify-every-wrs 32 --warmup 100 "${extra[@]}"
fi
