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
readonly SCRIPT_FULL_PATH=$(dirname $(readlink -f "$0"))
readonly PROJECT_FULL_PATH=$(dirname "$SCRIPT_FULL_PATH")

readonly BUILD_PATH="$PROJECT_FULL_PATH/build"
readonly OUTPUT_PATH="$PROJECT_FULL_PATH/output"
readonly HYBM_LIB_PATH="$OUTPUT_PATH/hybm/lib64"
readonly SMEM_LIB_PATH="$OUTPUT_PATH/smem/lib64"
readonly COVERAGE_PATH="$OUTPUT_PATH/coverage"
readonly TEST_REPORT_PATH="$OUTPUT_PATH/bin/gcover_report"
readonly MOCKCPP_PATH="$PROJECT_FULL_PATH/test/3rdparty/mockcpp"
readonly TEST_3RD_PATCH_PATH="$PROJECT_FULL_PATH/test/3rdparty/patch"
readonly MOCK_CANN_PATH="$HYBM_LIB_PATH/cann"
# BUILD_HCOM=ON 时 hcom(ubs-comm) 安装到 output/3rdparty/hcom/lib，HOST_TCP/HOST_RDMA 数据面
# 依赖运行期 dlopen("libhcom.so")，必须加入 LD_LIBRARY_PATH
readonly HCOM_LIB_PATH="$OUTPUT_PATH/3rdparty/hcom/lib"
readonly FINGERPRINT_FILE="$BUILD_PATH/.build_fingerprint"
: "${MF_UT_BUILD_TYPE:=ASAN}"
readonly BUILD_FINGERPRINT="${MF_UT_BUILD_TYPE}-UT-OPEN_ABI-HCOM"
# Default to 60% of nproc (leave headroom per TTFHW constraint); user can override via env var.
# Unset OMP_NUM_THREADS to get the real CPU count (some images set OMP_NUM_THREADS=1 which caps nproc).
if [ -z "${MF_BUILD_JOBS:-}" ]; then
    NPROC_COUNT=$(env -u OMP_NUM_THREADS nproc 2>/dev/null) || NPROC_COUNT=2
    : "${NPROC_COUNT:=2}"
    MF_BUILD_JOBS=$(( NPROC_COUNT * 60 / 100 ))
    [ "$MF_BUILD_JOBS" -lt 1 ] && MF_BUILD_JOBS=1
fi
readonly MF_BUILD_JOBS="$MF_BUILD_JOBS"

FAST_MODE=false
if [[ "$1" == "--fast" ]]; then
    FAST_MODE=true
    shift
fi
TEST_FILTER="*$1*"
cd ${PROJECT_FULL_PATH}
if $FAST_MODE; then
    NEED_FULL_BUILD=false
    if [ ! -d "${BUILD_PATH}" ] || [ ! -f "${FINGERPRINT_FILE}" ]; then
        echo "========= first build, full build ============"
        NEED_FULL_BUILD=true
    elif [ "$(cat ${FINGERPRINT_FILE} 2>/dev/null)" != "${BUILD_FINGERPRINT}" ]; then
        echo "========= build config changed, full rebuild ============"
        NEED_FULL_BUILD=true
    else
        echo "========= incremental build ============"
    fi
    if $NEED_FULL_BUILD; then
        rm -rf ${BUILD_PATH}
        rm -rf ${OUTPUT_PATH}
    fi
else
    rm -rf ${COVERAGE_PATH}
    rm -rf ${BUILD_PATH}
    rm -rf ${OUTPUT_PATH}
    rm -rf ${TEST_REPORT_PATH}
fi
mkdir -p ${BUILD_PATH}
mkdir -p ${TEST_REPORT_PATH}
mkdir -p ${OUTPUT_PATH}

set -e

export MF_HYBM_RDMA_SWAP_SPACE_SIZE=256

echo "========= UT env =========="
echo "MF_HYBM_RDMA_SWAP_SPACE_SIZE=${MF_HYBM_RDMA_SWAP_SPACE_SIZE:-<unset>}"

# dos2unix 对已是 Unix 格式的文件也会重写（mtime 变化），--fast 增量构建下会反复触发
# mockcpp 相关对象重编；先检测是否真的含 CRLF，仅在实际需要转换时才重写
convert_if_needed() {
    for f in "$@"; do
        if [ -f "$f" ] && grep -q $'\r' "$f"; then
            dos2unix "$f"
        fi
    done
}
convert_if_needed \
    "$MOCKCPP_PATH/include/mockcpp/JmpCode.h" \
    "$MOCKCPP_PATH/include/mockcpp/mockcpp.h" \
    "$MOCKCPP_PATH/src/JmpCode.cpp" \
    "$MOCKCPP_PATH/src/JmpCodeArch.h" \
    "$MOCKCPP_PATH/src/JmpCodeX64.h" \
    "$MOCKCPP_PATH/src/JmpCodeX86.h" \
    "$MOCKCPP_PATH/src/JmpOnlyApiHook.cpp" \
    "$MOCKCPP_PATH/src/UnixCodeModifier.cpp"
convert_if_needed $TEST_3RD_PATCH_PATH/*.patch
if command -v ninja &> /dev/null; then
    echo "========= build by ninja ============"
    export GENERATOR="Ninja"
    export MAKE_CMD=ninja
else
    GENERATOR="Unix Makefiles"
    export MAKE_CMD=make
fi
if ! $FAST_MODE || [ ! -f "${BUILD_PATH}/build.ninja" -a ! -f "${BUILD_PATH}/Makefile" ]; then
    cmake -G "$GENERATOR" -DCMAKE_BUILD_TYPE=${MF_UT_BUILD_TYPE} -DBUILD_UT=ON -DBUILD_OPEN_ABI=ON \
        -DBUILD_HCOM=ON -DBUILD_WITH_RDMA=ON -DBUILD_WITH_UB=OFF -S . -B ${BUILD_PATH}
    $FAST_MODE && echo "${BUILD_FINGERPRINT}" > "${FINGERPRINT_FILE}"
fi
${MAKE_CMD} install -j"${MF_BUILD_JOBS}" -C ${BUILD_PATH}
# ubs-comm 的 libhcom.so 运行期依赖 libboundscheck.so，但其 cmake install 不包含它
# （仅安装 libhcom_static.a / libhcom.so.*），需从 FetchContent 的 dist 产物补拷，
# 否则 dlopen("libhcom.so") 报 "libboundscheck.so: cannot open shared object file"
BOUNDS_CHECK_LIB="$BUILD_PATH/_deps/hcom-src/dist/hcom_3rdparty/libboundscheck/lib/libboundscheck.so"
if [ -f "$BOUNDS_CHECK_LIB" ]; then
    cp -f "$BOUNDS_CHECK_LIB" "$HCOM_LIB_PATH/"
fi
export LD_LIBRARY_PATH=$SMEM_LIB_PATH:$HYBM_LIB_PATH:$HCOM_LIB_PATH:$MOCK_CANN_PATH/driver/lib64
export ASCEND_HOME_PATH=$MOCK_CANN_PATH
export ASAN_OPTIONS="detect_stack_use_after_return=1:allow_user_poisoning=1"
export MF_HYBM_ENABLE_4K_PAGE=1

set +e
cd "$OUTPUT_PATH/bin/ut" && ./test_memfabric --gtest_output=xml:"$TEST_REPORT_PATH/test_detail.xml" --gtest_filter=${TEST_FILTER}
TEST_EXIT_CODE=$?
set -e

if [ ${TEST_EXIT_CODE} -ne 0 ]; then
    echo "Some test cases FAILED (exit code: ${TEST_EXIT_CODE})"
fi

if ! $FAST_MODE; then
    mkdir -p "$COVERAGE_PATH"
    cd "$OUTPUT_PATH"
    # 说明：BUILD_HCOM=ON 时 libhcom 由 FetchContent 拉到 build/_deps/hcom-src，它会被同一套
    # -fprofile-arcs -ftest-coverage 一起插桩（CMakeLists.txt:89-94 在 BUILD_UT=ON 时全局加覆盖选项，
    # 早于 :174 include(config_hcom.cmake) 触发的 FetchContent add_subdirectory），于是它的 130 个 TU
    # 会一并被 lcov 抓取，且路径含 "/src/" 能穿过上面的 "*/src/*" 过滤。它属第三方依赖、UT 只覆盖
    # 极小部分，计入分母会把整体比率从 ~70% 压到 ~38%，故整体排除（份额见下方 _deps 单独统计）。
    EXCLUDE_DIRS=(
            "*/3rdparty/*"
            "*/_deps/*"
            "*/src/smem/csrc/python_wrapper/*"
            "*/src/hybm/csrc/driver/*"
            "*/src/hybm/ops/*"
            "*/acc_links/csrc/common/*"
            "*/acc_links/csrc/security/*"
            "*/acc_links/csrc/under_api/openssl/*"
            "*/hybm/csrc/common/*"
            "*/hybm/csrc/ts_engine/*"
            "*/hybm/csrc/under_api/*"
            "*/src/hybm/csrc/transport/common/urma_topo_helper.cpp"
            "*/util/csrc/ptracer/tracers/*"
    )
    lcov --quiet -d "$BUILD_PATH" --c --output-file "$COVERAGE_PATH"/coverage.info -rc lcov_branch_coverage=1 --rc lcov_excl_br_line="LCOV_EXCL_BR_LINE|SM_LOG*|SM_ASSERT*|BM_LOG*|BM_ASSERT*|SM_VALIDATE_*|ASSERT*|LOG_*" --rc stop_on_error=0
    lcov --quiet -e "$COVERAGE_PATH"/coverage.info "*/src/*" -o "$COVERAGE_PATH"/coverage.info --rc lcov_branch_coverage=1 --rc stop_on_error=0 || true
    # third-party(_deps) 单独统计，仅用于确认份额，不计入门禁
    lcov --quiet -e "$COVERAGE_PATH"/coverage.info "*/_deps/*" -o "$COVERAGE_PATH"/coverage_deps.info \
        --rc lcov_branch_coverage=1 --rc stop_on_error=0 || true
    DEPS_SUMMARY=$(lcov --summary "$COVERAGE_PATH"/coverage_deps.info --rc lcov_branch_coverage=1 2>/dev/null || true)
    echo "third-party _deps (not gated): $(echo "$DEPS_SUMMARY" | grep -E 'lines|branches' | tr -s ' ' | tr '\n' ' ')"
    lcov --quiet -r "$COVERAGE_PATH"/coverage.info "${EXCLUDE_DIRS[@]}" -o "$COVERAGE_PATH"/coverage.info --rc lcov_branch_coverage=1 --rc stop_on_error=0 || true
    COV_SUMMARY=$(lcov -r "$COVERAGE_PATH"/coverage.info -o "$COVERAGE_PATH"/coverage.info --rc lcov_branch_coverage=1) || exit $?
    genhtml --quiet -o "$COVERAGE_PATH"/result "$COVERAGE_PATH"/coverage.info --show-details --legend --rc lcov_branch_coverage=1 --rc stop_on_error=0 || true

    lines_rate=$(echo "$COV_SUMMARY" | grep lines | grep -Eo "[0-9\.]+%" | tr -d '%')
    branches_rate=$(echo "$COV_SUMMARY" | grep branches | grep -Eo "[0-9\.]+%" | tr -d '%')
    echo "lines    coverage rate: ${lines_rate:-<unavailable>}%"
    echo "branches coverage rate: ${branches_rate:-<unavailable>}%"

    COVERAGE_FAILED=0
    if [ -z "${lines_rate}" ]; then
        echo "failed: lines coverage unavailable"
        COVERAGE_FAILED=1
    elif awk -v lines_rate="${lines_rate}" 'BEGIN { exit !(lines_rate < 70) }'; then
        echo "failed: lines coverage ${lines_rate}% < 70%"
        COVERAGE_FAILED=1
    fi

    if [ "${COVERAGE_FAILED}" -eq 0 ]; then
        if [ -z "${branches_rate}" ]; then
            echo "failed: branches coverage unavailable"
            COVERAGE_FAILED=1
        elif awk -v branches_rate="${branches_rate}" 'BEGIN { exit !(branches_rate < 40) }'; then
            echo "failed: branches coverage ${branches_rate}% < 40%"
            COVERAGE_FAILED=1
        fi
    fi
fi

if [ ${TEST_EXIT_CODE} -ne 0 ] || [ "${COVERAGE_FAILED:-0}" -ne 0 ]; then
    exit 1
else
    exit 0
fi
