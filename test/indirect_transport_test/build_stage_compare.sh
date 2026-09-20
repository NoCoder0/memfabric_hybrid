#!/usr/bin/env bash
# Build both HCOM linkage forms once under MF's existing build configuration.
set -euo pipefail
mf="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd -P)"
parent="$(dirname -- "$mf")"
ubs="${MF_HCOM_SOURCE_DIR:-${parent}/ubs-comm}"
sgl="${SGL_SOURCE_DIR:-${parent}/perf_test_duo_card_sgl}"
export MF_HCOM_SOURCE_DIR="$ubs" HCOM_SOURCE_DIR="$ubs"
cd "$mf"
# Forward the user's baseline ABI/XPU/Python options unchanged.
bash script/build_and_pack_run.sh "$@" --build_hcom ON --build_hcom_rdma ON
bash test/indirect_transport_test/build_hostrdma_bench.sh "$mf/output"
# Discover installed headers and both libraries from this ONE build, no standalone rebuild.
python3 test/indirect_transport_test/stage_build_manifest.py "$mf" "$ubs" "$sgl"
source "$mf/output/stage-build.env"
bash "$sgl/build.sh" --hcom-include "$STAGE_HCOM_INCLUDE" --hcom-lib "$STAGE_HCOM_LIB" \
    --boundscheck-root "$STAGE_BOUNDSCHECK_ROOT" --build-type Release
python3 test/indirect_transport_test/stage_build_manifest.py "$mf" "$ubs" "$sgl" --final
