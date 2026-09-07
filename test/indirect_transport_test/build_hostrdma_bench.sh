#!/bin/bash
# 一键编译 hostrdma_batch_bench（基于 MF output 产物）
# 用法：
#   bash build_hostrdma_bench.sh              # 编译到当前目录
#   bash build_hostrdma_bench.sh /path/output  # 指定 MF output 目录
set -e

SCRIPT_DIR=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
MF_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)   # 仓库根

# 1. 定位 MF output 目录（参数优先，否则仓库 output，否则环境变量）
if [ -n "$1" ]; then
    OUT="$1"
elif [ -d "${MF_ROOT}/output" ]; then
    OUT="${MF_ROOT}/output"
elif [ -n "${MF_OUTPUT_DIR}" ]; then
    OUT="${MF_OUTPUT_DIR}"
else
    echo "[ERROR] 找不到 MF output 目录，请传参：bash build_hostrdma_bench.sh <MF output 路径>"
    exit 1
fi
echo "MF output dir: ${OUT}"

# 2. 检查必需产物
SMEM_H="${OUT}/smem/include/host/smem_bm.h"
SMEM_LIB="${OUT}/smem/lib64/libmf_smem.so"
HYBM_LIB="${OUT}/hybm/lib64/libmf_hybm_core.so"
for f in "${SMEM_H}" "${SMEM_LIB}" "${HYBM_LIB}"; do
    if [ ! -f "$f" ]; then
        echo "[ERROR] 缺少: $f"
        echo "        请先编译 MF 包（cmake -DBUILD_HCOM=ON ...，产物在 output/）"
        exit 1
    fi
done

# 3. 编译
SRC="${SCRIPT_DIR}/hostrdma_batch_bench.cpp"
g++ -std=c++17 -O2 "${SRC}" \
    -I"${OUT}/smem/include/host" \
    -L"${OUT}/smem/lib64"     -lmf_smem \
    -L"${OUT}/hybm/lib64"     -lmf_hybm_core \
    -lpthread \
    -o "${SCRIPT_DIR}/hostrdma_batch_bench"
echo "[OK] 编译完成: ${SCRIPT_DIR}/hostrdma_batch_bench"

# 4. 运行指引
export LD_LIBRARY_PATH="${OUT}/smem/lib64:${OUT}/hybm/lib64:${LD_LIBRARY_PATH}"
if [ -d "${OUT}/3rdparty/hcom/lib" ]; then
    export LD_LIBRARY_PATH="${LD_LIBRARY_PATH}:${OUT}/3rdparty/hcom/lib"
fi

echo "---- ldd 检查 ----"
ldd "${SCRIPT_DIR}/hostrdma_batch_bench" || true
echo "---- 启动自检 ----"
"${SCRIPT_DIR}/hostrdma_batch_bench" --help && echo "[OK] 可运行，参数见上方 --help 输出" || echo "[提示] 缺动态库则把对应 so 目录加入 LD_LIBRARY_PATH"
