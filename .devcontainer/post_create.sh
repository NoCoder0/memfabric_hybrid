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
# post_create.sh — MemFabric Dev Container bootstrap
# =============================================================================
# This script runs once after the container is created.  It:
#   1. Initializes git submodules (third-party test dependencies).
#   2. Installs Python dev / test dependencies.
#   3. Verifies that the build toolchain is functional.
#   4. Runs a quick CMake configure smoke test.
#   5. Installs Git pre-commit hooks.
#
# VS Code's `postCreateCommand` in devcontainer.json invokes this file.
# Git submodule initialisation is performed in step 1 below (runs once
# after container creation; re-run manually if you switch branches).
#
# IMPORTANT — working directory:
#   Dev Containers run postCreateCommand in /workspaces/<repo-name> by default
#   (the mount point of the project root).  This script uses the PROJECT_DIR
#   variable, which resolves to the directory containing this .sh file's parent
#   (i.e. the repo root).  No hardcoded paths.
# =============================================================================

set -euo pipefail

# Resolve project root: the script lives at .devcontainer/post_create.sh,
# so its grandparent is the repo root.  No dependency on $PWD.
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPT_DIR}/.." && pwd)"

echo "============================================"
echo " MemFabric Dev Container — Post-Create Setup"
echo " Project dir: ${PROJECT_DIR}"
echo "============================================"

# ------------------------------------------------------------------
# 1. third party development dependencies
# ------------------------------------------------------------------


echo "[1/5] Initializing git submodules …"
git submodule update --init --recursive test/3rdparty/

# ------------------------------------------------------------------
# 2. Python development dependencies
# ------------------------------------------------------------------
echo ""
echo "[2/5] Installing Python development dependencies …"
if [ -f "${SCRIPT_DIR}/requirements.txt" ]; then
    pip3 install --no-cache-dir --break-system-packages \
        -r "${SCRIPT_DIR}/requirements.txt"
    echo "  ✓ Python dependencies installed."
else
    echo "  - requirements.txt not found — skipping."
fi

# ------------------------------------------------------------------
# 3. Verify toolchain
# ------------------------------------------------------------------
echo ""
echo "[3/5] Verifying build toolchain …"
echo "  gcc:  $(gcc --version | head -1)"
echo "  g++:  $(g++ --version | head -1)"
echo "  cmake: $(cmake --version | head -1)"
echo "  make: $(make --version | head -1)"
echo "  python: $(python3 --version)"
echo "  pip:  $(pip3 --version | head -1)"
echo "  ✓ Toolchain ready."

# ------------------------------------------------------------------
# 4. (Optional) Quick CMake configure smoke-test
# ------------------------------------------------------------------
echo ""
echo "[4/5] Quick CMake smoke test …"
if [[ -f /usr/local/Ascend/ascend-toolkit/set_env.sh ]]; then
    source /usr/local/Ascend/ascend-toolkit/set_env.sh
else
    echo "WARNING: Ascend toolkit env script not found, cmake may fail"
fi
cd "${PROJECT_DIR}"
cmake -S . -B /tmp/build-smoke \
    -DCMAKE_BUILD_TYPE=DEBUG \
    -DBUILD_UT=OFF \
    -DBUILD_PYTHON=OFF \
    -DBUILD_OPEN_ABI=OFF \
    -DENABLE_PTRACER=OFF \
    -DBUILD_TEST=OFF \
    -DBUILD_GIT_COMMIT=OFF \

rm -rf /tmp/build-smoke
echo "  ✓ CMake configure passed (smoke test)."

# ------------------------------------------------------------------
# 5. Install Git pre-commit hooks
# ------------------------------------------------------------------
echo ""
echo "[5/5] Installing pre-commit hooks …"
cd "${PROJECT_DIR}"
if [ -f ".pre-commit-config.yaml" ]; then
    pre-commit install
    echo "  ✓ pre-commit hooks installed."
else
    echo "  - .pre-commit-config.yaml not found — skipping."
fi

echo ""
echo "============================================"
echo " MemFabric Dev Container is ready!"
echo "============================================"
