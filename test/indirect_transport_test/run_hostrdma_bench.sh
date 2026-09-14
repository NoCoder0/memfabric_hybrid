#!/bin/bash
# 一键运行 hostrdma_batch_bench（86=local/87=remote）
# 用法：
#   86 机器: bash run_hostrdma_bench.sh local [mode]
#   87 机器: bash run_hostrdma_bench.sh remote [mode]
#   mode: all(默认) / baseline / cont
set -e

ROLE="$1"
MODE="${2:-all}"
if [ "${ROLE}" != "local" ] && [ "${ROLE}" != "remote" ]; then
    echo "用法: bash run_hostrdma_bench.sh <local|remote> [mode]"
    exit 1
fi

SCRIPT_DIR=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
BIN="${SCRIPT_DIR}/hostrdma_batch_bench"
if [ ! -x "${BIN}" ]; then
    echo "[ERROR] 先编译: bash ${SCRIPT_DIR}/build_hostrdma_bench.sh"
    exit 1
fi

# MF 库目录（先探测，与 build 脚本同一套逻辑）
MF=""
if [ -d "${SCRIPT_DIR}/../../output" ]; then
    MF="$(cd "${SCRIPT_DIR}/../.." && pwd)/output"
elif [ -d "/usr/local/memfabric_hybrid/latest/aarch64-linux" ]; then
    MF="/usr/local/memfabric_hybrid/latest/aarch64-linux"
fi
if [ -z "${MF}" ]; then
    echo "[WARN] 未探测到 MF 库目录，若运行时缺库请手动 export LD_LIBRARY_PATH"
else
    SMEM_LIB_DIR=$(dirname "$(find "${MF}" -name "libmf_smem.so" 2>/dev/null | head -1)")
    HYBM_LIB_DIR=$(dirname "$(find "${MF}" -name "libmf_hybm_core.so" 2>/dev/null | head -1)")
    HCOM_DIR=$(dirname "$(find "${MF}" -name "libhcom.so" 2>/dev/null | head -1)")
    export LD_LIBRARY_PATH="${SMEM_LIB_DIR}:${HYBM_LIB_DIR}:${HCOM_DIR}:${LD_LIBRARY_PATH}"
fi
export MF_HYBM_ENABLE_4K_PAGE=1

STORE_URL="tcp://90.91.183.86:18580"
# 第 3 个及之后的参数原样透传给 bench（例如 --rounds=100 --req-batch=1）
if [ "${ROLE}" == "local" ]; then
    # 86：local + 内嵌 config store
    exec "${BIN}" --role=local --rank=0 --store-url="${STORE_URL}" \
        --hcom-url="tcp://192.168.75.86:19000" --with-store=1 --mode="${MODE}" "${@:3}"
else
    # 87：remote
    exec "${BIN}" --role=remote --rank=1 --store-url="${STORE_URL}" \
        --hcom-url="tcp://192.168.75.87:19000" --mode="${MODE}" "${@:3}"
fi
