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


set -euo pipefail
if [ "$#" -lt 3 ]; then
    echo "Usage: $0 <absolute-library-directory> <device|host> <command> [args...]" >&2
    exit 1
fi
LIB_DIR=$(cd "$1" && pwd)
ROLE=$2
shift 2
for name in libmf_smem.so libmf_hybm_core.so libmf_opendcc.so; do
    test -f "${LIB_DIR}/${name}"
done
case "${ROLE}" in
    device) ;;
    host) : "${MF_OPENDCC_RDMA_IP:?Set the CX7 RoCE IP for this Host process}" ;;
    *) echo "Unknown role: ${ROLE}" >&2; exit 1 ;;
esac
export MF_HYBM_RDMA_USE_OPENDCC=1
export MF_HYBM_RDMA_USE_HCOMM=0
export MF_HYBM_RDMA_FORCE_UNREGISTERED=0
export MF_HYBM_RDMA_SWAP_SPACE_SIZE=0
export LD_LIBRARY_PATH="${LIB_DIR}${LD_LIBRARY_PATH:+:${LD_LIBRARY_PATH}}"
exec "$@"
