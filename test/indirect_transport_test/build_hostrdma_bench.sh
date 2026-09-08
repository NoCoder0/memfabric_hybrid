#!/bin/bash
# 一键编译 hostrdma_batch_bench（自动探测 MF 库/头位置）
# 用法：
#   bash build_hostrdma_bench.sh                     # 自动探测
#   bash build_hostrdma_bench.sh <MF 安装/输出根>     # 手动指定
set -e

SCRIPT_DIR=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
MF_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)   # 仓库根

# ---------- 1. 定位 MF 安装/输出根 ----------
MF=""
if [ -n "$1" ]; then
    MF="$1"
elif [ -d "${MF_ROOT}/output" ]; then
    MF="${MF_ROOT}/output"
elif [ -d "/usr/local/memfabric_hybrid/latest/aarch64-linux" ]; then
    MF="/usr/local/memfabric_hybrid/latest/aarch64-linux"
elif [ -n "${MF_INSTALL_DIR}" ]; then
    MF="${MF_INSTALL_DIR}"
else
    echo "[ERROR] 找不到 MF 库，请传参：bash build_hostrdma_bench.sh <MF 安装/输出根>"
    exit 1
fi
echo "MF root: ${MF}"

# ---------- 2. 在 MF 根下探测 头 与 库（兼容源码 output / 安装包布局） ----------
SMEM_H=$(find "${MF}" -name "smem_bm.h" 2>/dev/null | head -1)
SMEM_LIB=$(find "${MF}" -name "libmf_smem.so" 2>/dev/null | head -1)
HYBM_LIB=$(find "${MF}" -name "libmf_hybm_core.so" 2>/dev/null | head -1)
if [ -z "${SMEM_H}" ] || [ -z "${SMEM_LIB}" ] || [ -z "${HYBM_LIB}" ]; then
    echo "[ERROR] MF 产物不完整:"
    echo "  smem_bm.h       = ${SMEM_H:-缺失}"
    echo "  libmf_smem.so   = ${SMEM_LIB:-缺失}"
    echo "  libmf_hybm_core.so = ${HYBM_LIB:-缺失}"
    echo "请先编译/安装 MF（build_and_pack_run.sh --build_hcom ON ...）"
    exit 1
fi
SMEM_INC=$(dirname "${SMEM_H}")
SMEM_LIB_DIR=$(dirname "${SMEM_LIB}")
HYBM_LIB_DIR=$(dirname "${HYBM_LIB}")
echo "  header : ${SMEM_H}"
echo "  smem lib dir : ${SMEM_LIB_DIR}"
echo "  hybm lib dir : ${HYBM_LIB_DIR}"

# ---------- 3. 编译 ----------
SRC="${SCRIPT_DIR}/hostrdma_batch_bench.cpp"
g++ -std=c++17 -O2 "${SRC}" \
    -I"${SMEM_INC}" \
    -L"${SMEM_LIB_DIR}" -lmf_smem \
    -L"${HYBM_LIB_DIR}" -lmf_hybm_core \
    -lpthread \
    -o "${SCRIPT_DIR}/hostrdma_batch_bench"
echo "[OK] 编译完成: ${SCRIPT_DIR}/hostrdma_batch_bench"

# ---------- 4. 运行指引 ----------
export LD_LIBRARY_PATH="${SMEM_LIB_DIR}:${HYBM_LIB_DIR}:${LD_LIBRARY_PATH}"
HCOM_DIR=$(find "${MF}" -name "libhcom.so" 2>/dev/null | xargs -r dirname | head -1)
[ -n "${HCOM_DIR}" ] && export LD_LIBRARY_PATH="${HCOM_DIR}:${LD_LIBRARY_PATH}"
echo "---- ldd 检查 ----"
ldd "${SCRIPT_DIR}/hostrdma_batch_bench" || true
echo "---- 启动自检 ----"
"${SCRIPT_DIR}/hostrdma_batch_bench" --help && echo "[OK] 可运行" || echo "[提示] 缺库则把对应 so 目录加入 LD_LIBRARY_PATH"
