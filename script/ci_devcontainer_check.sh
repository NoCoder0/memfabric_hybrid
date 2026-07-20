#!/bin/bash
# Copyright (c) Huawei Technologies Co., Ltd. 2025-2025. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
# See the Mulan PSL v2 for more details.

# =============================================================================
# ci_devcontainer_check.sh -- CI validation via real Dockerfile + post_create.sh
# =============================================================================
# Reuses existing files directly -- does NOT re-implement the flow:
#   1. docker build  ->  .devcontainer/Dockerfile
#   2. docker run    ->  .devcontainer/post_create.sh   (inside container)
#   3. docker run    ->  script/run_all_examples.sh      (inside same container)
#
# Docker run args (mounts, devices, env) mirror .devcontainer/devcontainer.json.
#
# Usage:
#   bash script/ci_devcontainer_check.sh
#   bash script/ci_devcontainer_check.sh --help
#
# Exit codes:
#   0 = all steps passed
#   1 = prerequisite check or docker build/run failed
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

# -- Defaults ------------------------------------------------------------------
IMAGE_NAME="memfabric-ci:latest"

# -- Parse args ----------------------------------------------------------------
while [[ $# -gt 0 ]]; do
    case "$1" in
        --help)
            head -28 "$0" | sed -n '/^# Usage:/,/^# ===/p' | sed 's/^# //'
            exit 0
            ;;
        *)
            echo "[ERROR] Unknown option: $1 (use --help)"
            exit 1
            ;;
    esac
done

# -- Helpers -------------------------------------------------------------------
info()  { echo -e "  \e[36m[CI]\e[0m  $*"; }
ok()    { echo -e "  \e[32m[OK]\e[0m  $*"; }
err()   { echo -e "  \e[31m[ERR]\e[0m  $*"; }
header(){ echo -e "\n\e[1;34m=== $* ===\e[0m"; }

# -- Step 1: Check prerequisites -----------------------------------------------
check_prerequisites() {
    if ! command -v docker &>/dev/null; then
        err "docker not found. Install Docker or use a CI runner with Docker support."
        exit 1
    fi
    ok "docker: $(docker --version)"

    local npu_count
    npu_count=$(ls -d /dev/davinci[0-9]* 2>/dev/null | wc -l || true)
    if [[ "$npu_count" -eq 0 ]]; then
        err "No NPU devices found on host (/dev/davinci*)."
        exit 1
    fi
    ok "NPU: $npu_count device(s) on host"

    # Bind mount sources from devcontainer.json -- must exist on host
    local mount_paths=(
        /usr/local/dcmi
        /usr/local/Ascend/driver/tools/hccn_tool
        /usr/local/bin/npu-smi
        /usr/local/Ascend/driver/lib64/
        /usr/local/Ascend/driver/version.info
        /etc/ascend_install.info
        /etc/hccn.conf
    )
    for p in "${mount_paths[@]}"; do
        if [[ ! -e "$p" ]]; then
            err "Bind mount source not found: $p"
            err "Ensure CANN driver is installed on the host."
            exit 1
        fi
    done
    ok "All bind mount sources exist"
}

# -- Step 2: Build Docker image from .devcontainer/Dockerfile -------------------
build_image() {
    info "Building Docker image: $IMAGE_NAME"
    info "  Dockerfile: $PROJECT_DIR/.devcontainer/Dockerfile"

    docker build -t "$IMAGE_NAME" \
        -f "$PROJECT_DIR/.devcontainer/Dockerfile" \
        --network=host "$PROJECT_DIR"
    ok "Image built: $IMAGE_NAME"
}

# -- Step 3: Run container (mirrors devcontainer.json) -------------------------
run_in_container() {
    info "Starting container..."

    local inner="bash .devcontainer/post_create.sh"
    inner+=" && bash script/run_all_examples.sh"
    inner+=" --start-store --continue-on-error"

    # Mounts, devices, env from .devcontainer/devcontainer.json
    local run_args=(
        run --rm
        --network=host
        --privileged
        --device /dev/davinci_manager
        --device /dev/devmm_svm
        --device /dev/hisi_hdc
        -v "$PROJECT_DIR:/workspaces/memfabric_hybrid"
        -v /usr/local/dcmi:/usr/local/dcmi
        -v /usr/local/Ascend/driver/tools/hccn_tool:/usr/local/Ascend/driver/tools/hccn_tool
        -v /usr/local/bin/npu-smi:/usr/local/bin/npu-smi
        -v /usr/local/Ascend/driver/lib64/:/usr/local/Ascend/driver/lib64/
        -v /usr/local/Ascend/driver/version.info:/usr/local/Ascend/driver/version.info
        -v /etc/ascend_install.info:/etc/ascend_install.info
        -v /etc/hccn.conf:/etc/hccn.conf
        -v /home:/home
        -e PYTHONUNBUFFERED=1
        -e PIP_INDEX_URL=https://mirrors.aliyun.com/pypi/simple/
        -e PIP_TRUSTED_HOST=mirrors.aliyun.com
        -e PRE_COMMIT_HOME=/root/.cache/pre-commit
        -e PIP_SYSTEM_SITE_PACKAGES=1
        -e CMAKE_BUILD_PARALLEL_LEVEL=$(nproc)
        -e MMC_BUILD_JOBS=$(nproc)
        -e PYTHON_HOME=/usr/local/python3.11.15
        -w /workspaces/memfabric_hybrid
    )

    docker "${run_args[@]}" "$IMAGE_NAME" bash -c "$inner" || {
        rc=$?
        err "Container exited with code $rc"
        exit $rc
    }
    ok "Container run completed"
}

# -- Main ----------------------------------------------------------------------
echo ""
echo "============================================"
echo " MemFabric CI -- DevContainer Check"
echo " Project: $PROJECT_DIR"
echo " Image:   $IMAGE_NAME"
echo "============================================"

header "Step 1: Check prerequisites"
check_prerequisites

header "Step 2: Build Docker image (Dockerfile)"
build_image

header "Step 3: Run post_create.sh + run_all_examples.sh (in container)"
run_in_container

echo ""
echo "============================================"
echo " CI DevContainer Check -- PASSED"
echo "============================================"
