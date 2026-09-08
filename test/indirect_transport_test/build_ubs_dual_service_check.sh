#!/bin/bash
# 一键编译 ubs_dual_service_check（同进程双 Hcom service / 双卡链路最小验证，自动探测头文件）
# 用法：
#   bash build_ubs_dual_service_check.sh                    # 自动探测（优先仓库源码头）
#   bash build_ubs_dual_service_check.sh <MF 源码仓或 run 包输出根>   # 手动指定
set -e

SCRIPT_DIR=$(cd "$(dirname "$(readlink -f "$0")")" && pwd)
MF_ROOT=$(cd "${SCRIPT_DIR}/../.." && pwd)   # 仓库根（若在源码仓内）
SRC="${SCRIPT_DIR}/ubs_dual_service_check.cpp"
OUT="${SCRIPT_DIR}/ubs_dual_service_check"
[ -f "${SRC}" ] || { echo "[ERROR] 缺少源文件: ${SRC}"; exit 1; }

# ---------- 1. 定位 hcom 类型头（hcom_service_c_define.h / hcom_c_define.h） ----------
HDR_DIR=""
CAND=()
[ -n "$1" ] && CAND+=("$1")
[ -d "${MF_ROOT}/src/hybm/csrc/under_api" ] && CAND+=("${MF_ROOT}/src/hybm/csrc/under_api")
[ -d "${MF_ROOT}/output" ] && CAND+=("${MF_ROOT}/output")
[ -n "${MF_INSTALL_DIR}" ] && CAND+=("${MF_INSTALL_DIR}")
for d in "${CAND[@]}"; do
    f=$(find "${d}" -name "hcom_service_c_define.h" 2>/dev/null | head -1)
    if [ -n "${f}" ]; then
        HDR_DIR=$(dirname "${f}")
        break
    fi
done
if [ -z "${HDR_DIR}" ]; then
    echo "[ERROR] 找不到 hcom_service_c_define.h，请传参：bash build_ubs_dual_service_check.sh <源码仓或run包根>"
    exit 1
fi
echo "hcom 头目录: ${HDR_DIR}"

# ---------- 2. 编译 ----------
g++ -std=c++17 -O2 "${SRC}" -I"${HDR_DIR}" -ldl -o "${OUT}"
echo "[OK] 编译完成: ${OUT}"

# ---------- 3. 运行环境自检 ----------
HCOM_SO=$(find "${MF_ROOT}" -name "libhcom.so" 2>/dev/null | head -1)
if [ -z "${HCOM_SO}" ] && [ -n "$1" ]; then
    HCOM_SO=$(find "$1" -name "libhcom.so" 2>/dev/null | head -1)
fi
if [ -n "${HCOM_SO}" ]; then
    HCOM_DIR=$(dirname "${HCOM_SO}")
    echo "libhcom.so 目录: ${HCOM_DIR}（运行时加入 LD_LIBRARY_PATH）"
    echo "  export LD_LIBRARY_PATH=${HCOM_DIR}:\${LD_LIBRARY_PATH}"
else
    echo "[提示] 未在仓库内找到 libhcom.so；运行时请把其目录加入 LD_LIBRARY_PATH"
fi

echo "---- 运行示例（87=listen，86=conn，端口可改）----"
echo "  87: bash ${SCRIPT_DIR}/build_ubs_dual_service_check.sh && ./ubs_dual_service_check --listen --ip1 192.168.75.87 --ip2 192.168.65.87 --p1 18586 --p2 18686"
echo "  86: bash ${SCRIPT_DIR}/build_ubs_dual_service_check.sh && ./ubs_dual_service_check --conn   --ip1 192.168.75.86 --ip2 192.168.65.86 --p1 18586 --p2 18686 --peer1 tcp://192.168.75.87:18586 --peer2 tcp://192.168.65.87:18686"
