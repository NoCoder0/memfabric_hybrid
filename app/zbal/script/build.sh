#!/bin/bash
set -e

DEBUG_MODE="OFF"
ENABLE_ZBAL_UT="OFF"
BUILD_FUSED_DEEP_MOE="ON"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --debug)
            DEBUG_MODE="ON"
            shift
            ;;
        --ut)
            ENABLE_ZBAL_UT="ON"
            shift
            ;;
        --skip-fused-moe)
            BUILD_FUSED_DEEP_MOE="OFF"
            shift
            ;;
        -h|--help)
            echo "Usage: $0 [options]"
            echo "Options:"
            echo "  --debug           Enable debug mode"
            echo "  --ut              Enable unit tests"
            echo "  --skip-fused-moe  Skip fused_deep_moe compilation (A3 only, implies --debug)"
            echo "  -h, --help        Show this help message"
            exit 1
            ;;
        *)
            echo "Error: unknown option: $1" 1>&2
            echo "Run '$0 -h' for more information."
            exit 1
            ;;
    esac
done

if [[ "${BUILD_FUSED_DEEP_MOE}" == "OFF" && "${DEBUG_MODE}" == "OFF" ]]; then
    echo "[build.sh] --skip-fused-moe implies --debug"
    DEBUG_MODE="ON"
fi

export DEBUG_MODE=$DEBUG_MODE
export ENABLE_ZBAL_UT=$ENABLE_ZBAL_UT
export BUILD_FUSED_DEEP_MOE=$BUILD_FUSED_DEEP_MOE

if [ -n "$ASCEND_HOME_PATH" ]; then
    _ASCEND_INSTALL_PATH=$ASCEND_HOME_PATH
else
    _ASCEND_INSTALL_PATH=/usr/local/Ascend/ascend-toolkit/latest
fi

if [ -n "$ASCEND_INCLUDE_DIR" ]; then
    ASCEND_INCLUDE_DIR=$ASCEND_INCLUDE_DIR
else
    ASCEND_INCLUDE_DIR=${_ASCEND_INSTALL_PATH}/aarch64-linux/include
fi

export ASCEND_TOOLKIT_HOME=${_ASCEND_INSTALL_PATH}
export ASCEND_HOME_PATH=${_ASCEND_INSTALL_PATH}
echo "ascend path: ${ASCEND_HOME_PATH}"
source ${ASCEND_HOME_PATH}/set_env.sh

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
PROJECT_ROOT=$(dirname "$SCRIPT_DIR")
OUTPUT_DIR=${PROJECT_ROOT}/output
rm -rf ${OUTPUT_DIR}
mkdir -p $OUTPUT_DIR
echo "outpath: ${OUTPUT_DIR}"

COMPILE_OPTIONS=""

function build_zbal()
{
    echo "[zbal] Building zbal via setup.py"
    cd ${PROJECT_ROOT}/src/python || exit
    rm -rf build output dist memfabric_zbal.*
    python3 setup.py bdist_wheel
    mv -v dist/memfabric_zbal*.whl "${OUTPUT_DIR}/"
    rm -rf dist
    cd -
}

function main()
{
    if pip3 show wheel;then
        echo "wheel has been installed"
    else
        pip3 install wheel==0.45.1
    fi
    build_zbal
}

main
