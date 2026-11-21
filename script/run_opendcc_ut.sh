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
ROOT_PATH=$(cd "$(dirname "$0")/.." && pwd)
BUILD_PATH="${ROOT_PATH}/build-opendcc-ut"
cd "${ROOT_PATH}"
for source in include/mockcpp/JmpCode.h include/mockcpp/mockcpp.h src/JmpCode.cpp src/JmpCodeArch.h \
              src/JmpCodeX64.h src/JmpCodeX86.h src/JmpOnlyApiHook.cpp src/UnixCodeModifier.cpp; do
    path="test/3rdparty/mockcpp/${source}"
    if grep -q $'\r' "${path}"; then dos2unix "${path}"; fi
done
cmake -S . -B "${BUILD_PATH}" -DXPU_TYPE=NONE -DBUILD_PYTHON=OFF -DBUILD_HCOM=OFF \
    -DBUILD_OPENDCC=ON -DBUILD_UT=ON -DCMAKE_BUILD_TYPE="${MF_UT_BUILD_TYPE:-DEBUG}"
cmake --build "${BUILD_PATH}" --target test_openDCC -j"${MF_BUILD_JOBS:-8}"
"${BUILD_PATH}/test/ut/testcase/openDCC/test_openDCC" "--gtest_filter=${1:-*}"
