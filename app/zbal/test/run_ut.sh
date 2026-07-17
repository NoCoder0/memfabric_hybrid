#!/bin/bash

set -e

CURRENT_DIR=$(cd $(dirname ${BASH_SOURCE:-$0}) && pwd)
ZBAL_DIR=$(cd ${CURRENT_DIR}/.. && pwd)
SGL_KERNEL_DIR=$(cd ${ZBAL_DIR}/../.. && pwd)

BUILD_DIR=${ZBAL_DIR}/test/build
OUTPUT_DIR=${ZBAL_DIR}/test
MOCK_LIB_DIR=${OUTPUT_DIR}/lib64/cann/lib64
COVERAGE_PATH="$OUTPUT_DIR/coverage"

rm -rf $COVERAGE_PATH

# =============================================
# Environment setup
# =============================================
if [ -n "$ASCEND_HOME_PATH" ]; then
    _ASCEND_INSTALL_PATH=$ASCEND_HOME_PATH
else
    _ASCEND_INSTALL_PATH=/usr/local/Ascend/ascend-toolkit/latest
fi
export ASCEND_HOME_PATH=${ASCEND_HOME_PATH:-/usr/local/Ascend}
SOC_VERSION="Ascend910_9382"
export SOC_VERSION="${SOC_VERSION}"

# UT binary is produced by build.sh --ut. Do not compile here.
TEST_BIN=${BUILD_DIR}/test/ut/testcase/test_zbal
if [ ! -x "${TEST_BIN}" ]; then
    echo "[ERROR] UT binary not found: ${TEST_BIN}"
    echo "[ERROR] Run 'script/build.sh --ut' first to compile the UT."
    exit 1
fi

# Set MOCK LD_LIBRARY_PATH for runtime
export LD_LIBRARY_PATH=${MOCK_LIB_DIR}:${ASCEND_HOME_PATH}/runtime/lib64:${ASCEND_HOME_PATH}/lib64:${LD_LIBRARY_PATH}

# Run unit tests
echo "[INFO] Running unit tests..."
${TEST_BIN} "$@"

echo "[INFO] generate coverage rate..."
mkdir -p "$COVERAGE_PATH"

if ! command -v lcov &> /dev/null; then
    echo "[ERROR] lcov is not installed, generate coverage rate failed. Exit..."
    exit 1
fi

EXCLUDE_DIRS=(
        "*/3rdparty/*"
        "*/doc/*"
        "*/python/*"
        "*/test/*"
        "*/under_api/*"
        "*/device/*"
        "*/common/zbal_logger.h"
        "*/common/zbal_last_error.h"
)
echo "[INFO] generate coverage rate... BUILD_DIR: ${BUILD_DIR}  COVERAGE_PATH:${COVERAGE_PATH}"
lcov -c -d "$BUILD_DIR" \
    --output-file "$COVERAGE_PATH"/coverage.info \
    --rc lcov_branch_coverage=1 \
    --rc lcov_excl_br_line="LCOV_EXCL_BR_LINE|ZBAL_LOG*|ZBAL_ASSERT*|ZBAL_CHECK*|ASCEND_LOG*|ZBAL_VALIDATE*|ZBAL_OUT_LOG*|ZBAL_OP_LOG*|ZBAL_UNLIKELY*|ZBAL_LIKELY*" \
    --rc stop_on_error=0 \

lcov -e "$COVERAGE_PATH"/coverage.info "*/src/*" -o "$COVERAGE_PATH"/coverage.info --rc lcov_branch_coverage=1 --rc stop_on_error=0 || true
lcov -r "$COVERAGE_PATH"/coverage.info "${EXCLUDE_DIRS[@]}" -o "$COVERAGE_PATH"/coverage.info --rc lcov_branch_coverage=1 --rc stop_on_error=0 || true
genhtml -o "$COVERAGE_PATH"/result "$COVERAGE_PATH"/coverage.info --show-details --legend --rc lcov_branch_coverage=1 --rc stop_on_error=0 || true

summary=$(lcov --summary "$COVERAGE_PATH"/coverage.info --rc lcov_branch_coverage=1 --rc stop_on_error=0)
echo "$summary"
lines_rate=$(echo "$summary" | grep lines | grep -Eo "[0-9\.]+%" | tr -d '%')
branches_rate=$(echo "$summary" | grep branches | grep -Eo "[0-9\.]+%" | tr -d '%')
echo "lines    coverage rate: ${lines_rate}%"
echo "branches coverage rate: ${branches_rate}%"

if [[ $(awk "BEGIN {print (${lines_rate} < 70 || ${branches_rate} < 40) ? 1 : 0}") -eq 1 ]]; then
    echo "failed: lines coverage < 70% or branches coverage < 40%"
    exit -1
else
    exit 0
fi
