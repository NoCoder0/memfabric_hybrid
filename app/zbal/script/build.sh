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
            echo "  --ut              Enable unit tests (skips wheel build)"
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

function update_ut_submodules()
{
    local repo_root
    repo_root=$(git -C "${SCRIPT_DIR}" rev-parse --show-toplevel 2>/dev/null)
    if [ -z "${repo_root}" ]; then
        echo "[build.sh] not a git repo, skip submodule update" 1>&2
        return 0
    fi
    echo "[build.sh] --ut: syncing and updating submodules from .gitmodules"
    git -C "${repo_root}" submodule sync
    git -C "${repo_root}" submodule update --init --recursive \
        --jobs "$(nproc 2>/dev/null || echo 8)"

    local zbal_3rdparty="${PROJECT_ROOT}/test/3rdparty"
    local sm_path sm_name src
    while read -r _ sm_path; do
        sm_name=$(basename "${sm_path}")
        src="${repo_root}/${sm_path}"
        if [ ! -d "${src}" ]; then
            echo "[build.sh] warn: submodule missing: ${src}" 1>&2
            continue
        fi
        ln -sfn "${src}" "${zbal_3rdparty}/${sm_name}"
        echo "[build.sh] linked ${zbal_3rdparty}/${sm_name} -> ${src}"
    done < <(git -C "${repo_root}" config -f .gitmodules --get-regexp '\.path$')
}

function build_zbal()
{
    echo "[zbal] Building zbal via setup.py"
    cd ${PROJECT_ROOT}/src/python || exit
    rm -rf build output dist memfabric_zbal.*
    ENABLE_ZBAL_UT=OFF python3 setup.py bdist_wheel
    mv -v dist/memfabric_zbal*.whl "${OUTPUT_DIR}/"
    rm -rf dist
    cd -
}

function build_ut()
{
    local zbal_dir="${PROJECT_ROOT}"
    local build_dir="${zbal_dir}/test/build"
    local soc_version="Ascend910_9382"

    local chip_type="A3"
    case "${soc_version}" in
        [Aa]scend950*) chip_type="A5" ;;
    esac
    local chip_macro="ZBAL_ASCEND_NPU_${chip_type}"

    # Build GoogleTest if not already built
    local gtest_lib="${zbal_dir}/test/output/3rdparty/googletest/lib/libgtest.a"
    if [ ! -f "${gtest_lib}" ]; then
        echo "[build.sh] Building GoogleTest..."
        local gtest_src="${zbal_dir}/test/3rdparty/googletest"
        local gtest_build="${gtest_src}/build"
        local gtest_install="${zbal_dir}/test/output/3rdparty/googletest"
        mkdir -p "${gtest_build}" "${gtest_install}"
        cmake -S "${gtest_src}" -B "${gtest_build}" \
            -DCMAKE_C_COMPILER=gcc -DCMAKE_CXX_COMPILER=g++ \
            -DCMAKE_BUILD_TYPE=DEBUG \
            -DCMAKE_INSTALL_PREFIX="${gtest_install}/"
        cmake --build "${gtest_build}" --target install -j8
        echo "[build.sh] GoogleTest built successfully."
    fi

    echo "[build.sh] Configuring UT build... (${chip_macro}, BUILD_FUSED_DEEP_MOE=OFF)"
    rm -rf "${build_dir}"
    mkdir -p "${build_dir}"
    cmake -S "${zbal_dir}" -B "${build_dir}" \
        -DBUILD_ZBAL_MODULE_UT=ON \
        -DSOC_VERSION="${soc_version}" \
        -D${chip_macro}=1 \
        -DBUILD_FUSED_DEEP_MOE=OFF \
        -DCMAKE_BUILD_TYPE=DEBUG \
        -DDISABLE_ADAPTOR_COMPILE=ON \
        -DDISABLE_ALLOCATOR_COMPILE=ON \
        -DDISABLE_SHARE_COMPILE=ON \
        -DCMAKE_CXX_FLAGS="--coverage -fprofile-update=atomic" \
        -DCMAKE_C_FLAGS="--coverage -fprofile-update=atomic" \
        -DCMAKE_EXE_LINKER_FLAGS="--coverage" \
        -DCMAKE_SHARED_LINKER_FLAGS="--coverage"

    echo "[build.sh] Building UT targets..."
    cmake --build "${build_dir}" --target test_zbal acl_shared -j8
    echo "[build.sh] UT binary: ${build_dir}/test/ut/testcase/test_zbal"
}

function main()
{
    if pip3 show wheel;then
        echo "wheel has been installed"
    else
        pip3 install wheel==0.45.1
    fi
    if [[ "${ENABLE_ZBAL_UT}" == "ON" ]]; then
        update_ut_submodules
        # --ut: skip wheel build, only build UT targets
        build_ut
    else
        build_zbal
    fi
}

main
