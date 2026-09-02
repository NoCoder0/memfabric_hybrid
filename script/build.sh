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

export BUILD_MODE=${1:-RELEASE}
BUILD_UT=${2:-OFF}
BUILD_OPEN_ABI=${3:-OFF}
BUILD_PYTHON=${4:-ON}
ENABLE_PTRACER=${5:-ON}
export XPU_TYPE=${6:-NPU} # 导出环境变量用于后续构建whl包
BUILD_TEST=${7:-OFF}
BUILD_HCOM=${8:-OFF}
BUILD_HCOM_WITH_RDMA=${9:-ON}
BUILD_HCOM_WITH_UB=${10:-OFF}
BUILD_ETCD_BACKEND=${11:-OFF}
BUILD_TOOL=${12:-cmake}
# 导出环境变量用于后续构建whl包
export MF_BUILD_HCOM=${8:-OFF}
export MF_BUILD_HCOM_WITH_RDMA=${9:-ON}
export MF_BUILD_HCOM_WITH_UB=${10:-OFF}
export BUILD_ETCD_BACKEND=${11:-OFF}
export BUILD_TOOL=${12:-cmake}


readonly SCRIPT_FULL_PATH=$(dirname $(readlink -f "$0"))
readonly PROJECT_FULL_PATH=$(dirname "$SCRIPT_FULL_PATH")
readonly MIN_CANN_VERSION="9.1.0"
readonly MF_BUILD_JOBS="${MF_BUILD_JOBS:-32}"

if [ "${BUILD_UT}" == "ON" ]; then
  readonly MOCKCPP_PATH="$PROJECT_FULL_PATH/test/3rdparty/mockcpp"
  readonly TEST_3RD_PATCH_PATH="$PROJECT_FULL_PATH/test/3rdparty/patch"
  dos2unix "$MOCKCPP_PATH/include/mockcpp/JmpCode.h"
  dos2unix "$MOCKCPP_PATH/include/mockcpp/mockcpp.h"
  dos2unix "$MOCKCPP_PATH/src/JmpCode.cpp"
  dos2unix "$MOCKCPP_PATH/src/JmpCodeArch.h"
  dos2unix "$MOCKCPP_PATH/src/JmpCodeX64.h"
  dos2unix "$MOCKCPP_PATH/src/JmpCodeX86.h"
  dos2unix "$MOCKCPP_PATH/src/JmpOnlyApiHook.cpp"
  dos2unix "$MOCKCPP_PATH/src/UnixCodeModifier.cpp"
  dos2unix $TEST_3RD_PATCH_PATH/*.patch
fi

set -e
readonly ROOT_PATH=$(dirname $(readlink -f "$0"))
CURRENT_DIR=$(pwd)

copy_bazel_artifacts()
{
    echo "========= copy bazel artifacts to dist directory========="
    # copy from bazel-bin to output
    mkdir -p ${PROJ_DIR}/output/smem/lib64/
    cp -v "${PROJ_DIR}/bazel-bin/src/smem/csrc/libmf_smem.so" "${PROJ_DIR}/output/smem/lib64/"
    mkdir -p ${PROJ_DIR}/output/hybm/lib64/
    cp -v "${PROJ_DIR}/bazel-bin/src/hybm/csrc/libmf_hybm_core.so" "${PROJ_DIR}/output/hybm/lib64/"
    mkdir -p ${PROJ_DIR}/output/acc_offload/lib64/
    cp -v "${PROJ_DIR}/bazel-bin/src/acc_offload/csrc/libmf_acc_offload.so" "${PROJ_DIR}/output/acc_offload/lib64/"
    mkdir -p ${PROJ_DIR}/output/smem/include/host/
    cp -v "${PROJ_DIR}/src/smem/include/host/"*.h "${PROJ_DIR}/output/smem/include/host/"
    mkdir -p ${PROJ_DIR}/output/smem/include/device/
    cp -v "${PROJ_DIR}/src/smem/include/device/"*.h "${PROJ_DIR}/output/smem/include/device/"
    mkdir -p ${PROJ_DIR}/output/hybm/include/
    cp -v "${PROJ_DIR}/src/hybm/include/"*.h "${PROJ_DIR}/output/hybm/include/"
    mkdir -p ${PROJ_DIR}/output/acc_offload/include/host/
    cp -v "${PROJ_DIR}/src/acc_offload/include/host/"*.h "${PROJ_DIR}/output/acc_offload/include/host/"

    # copy from bazel-bin to build
    if [ "${BUILD_PYTHON}" == "ON" ]; then
        mkdir -p "${PROJ_DIR}"/build/src/smem/csrc/python_wrapper/mk_transfer_adapter/
        cp -v "${PROJ_DIR}"/bazel-bin/src/smem/csrc/python_wrapper/mk_transfer_adapter/_pymf_transfer.so "${PROJ_DIR}"/build/src/smem/csrc/python_wrapper/mk_transfer_adapter/
        mkdir -p "${PROJ_DIR}"/build/src/smem/csrc/python_wrapper/memfabric_hybrid/
        cp -v "${PROJ_DIR}"/bazel-bin/src/smem/csrc/python_wrapper/memfabric_hybrid/_pymf_hybrid.so "${PROJ_DIR}"/build/src/smem/csrc/python_wrapper/memfabric_hybrid/
        mkdir -p "${PROJ_DIR}"/build/src/acc_offload/csrc/python_wrapper/
        cp -v "${PROJ_DIR}"/bazel-bin/src/acc_offload/csrc/python_wrapper/_pymf_acc_offload.so "${PROJ_DIR}"/build/src/acc_offload/csrc/python_wrapper/
    fi

    if [ "${BUILD_HCOM}" == "ON" ]; then
        echo "========= copy hcom lib to output============"
        mkdir -p "${PROJ_DIR}"/output/3rdparty/hcom/lib/
        cp -v "${PROJ_DIR}"/bazel-bin/external/hcom/src/hcom/libhcom.so "${PROJ_DIR}"/output/3rdparty/hcom/lib/

        if [ -f "${PROJ_DIR}/bazel-bin/external/hcom/src/libboundscheck.so" ]; then
            if [ ! -d "$LIBBOUNDSCHECK_INSTALL_PATH" ]; then
                mkdir -p "$LIBBOUNDSCHECK_INSTALL_PATH"
            fi
            cp -v ${PROJ_DIR}/bazel-bin/external/hcom/src/libboundscheck.so "$LIBBOUNDSCHECK_INSTALL_PATH"
        fi
    fi

    if [ "${BUILD_ETCD_BACKEND}" == "ON" ]; then
        echo "========= copy etcd client lib to output============"
        mkdir -p ${PROJ_DIR}/output/etcd/lib64/
        cp -v "${PROJ_DIR}"/bazel-bin/src/util/etcd_client/etcd_store_backend/libetcd_client_v3.so ${PROJ_DIR}/output/etcd/lib64/
    fi
}

check_contains_path()
{
    local check_path="$1"
    local path_var="${LD_LIBRARY_PATH:-}"

    # empty LD_LIBRARY_PATH
    if [ -z "$path_var" ]; then
        return 0 # not contain
    fi

    # check every path parted by ':'
    IFS=':' read -ra paths <<< "$path_var"
    for path in "${paths[@]}"; do
        # remove '/'
        path="${path%/}"
        check_path="${check_path%/}"

        if [ "$path" = "$check_path" ]; then
            echo "========= contain $check_path============"
            return 1  # contain
        fi
    done
    echo "========= not contain $check_path============"
    return 0  # not contain
}

get_cann_version()
{
    local cann_root="$1"
    local version_file
    local version
    local candidates=(
        "${cann_root}/$(uname -m)-linux/ascend_toolkit_install.info"
        "${cann_root}/aarch64-linux/ascend_toolkit_install.info"
        "${cann_root}/x86_64-linux/ascend_toolkit_install.info"
        "${cann_root}/ascend_toolkit_install.info"
        "${cann_root}/version.cfg"
    )
    for version_file in "${candidates[@]}"; do
        if [ ! -f "${version_file}" ]; then
            continue
        fi
        version="$(awk -F= '
            tolower($1) ~ /^[[:space:]]*version[[:space:]]*$/ {
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2)
                print $2
                exit
            }
        ' "${version_file}")"
        if [ -n "${version}" ]; then
            echo "${version}"
            return 0
        fi
    done
    return 1
}

is_cann_version_supported()
{
    local version="$1"
    if [[ ! "${version}" =~ ^[^0-9]*([0-9]+)\.([0-9]+)(\.([0-9]+))? ]]; then
        return 1
    fi
    local major=$((10#${BASH_REMATCH[1]}))
    local minor=$((10#${BASH_REMATCH[2]}))
    local patch=$((10#${BASH_REMATCH[4]:-0}))
    (( major > 9 || (major == 9 && minor > 1) || (major == 9 && minor == 1 && patch >= 0) ))
}

build_hybm_ops()
{
    local ascend_root="${ASCEND_HOME_PATH:-}"
    local aicpu_compiler="${ascend_root}/toolkit/toolchain/hcc/bin/aarch64-target-linux-gnu-g++"
    local ascend_include="${ascend_root}/aarch64-linux/pkg_inc/base"
    local ops_build_dir="${PROJ_DIR}/build/hybm_ops"
    local ops_output_dir="${PROJ_DIR}/output/hybm/aicpu_kernel"

    if [ "${XPU_TYPE}" != "NPU" ]; then
        echo "========= skip build HYBM OPS: XPU_TYPE is ${XPU_TYPE} ============"
        return 0
    fi
    if [ -z "${ascend_root}" ] || [ ! -d "${ascend_root}" ]; then
        echo "========= skip build HYBM OPS: CANN environment is unavailable ============"
        return 0
    fi
    local cann_version
    if ! cann_version="$(get_cann_version "${ascend_root}")"; then
        echo "========= skip build HYBM OPS: cannot detect CANN version (required >= ${MIN_CANN_VERSION}) ============"
        return 0
    fi
    if ! is_cann_version_supported "${cann_version}"; then
        echo "========= skip build HYBM OPS: CANN ${cann_version} does not meet >= ${MIN_CANN_VERSION} ============"
        return 0
    fi
    echo "========= detected CANN ${cann_version} ============"
    if [ ! -x "${aicpu_compiler}" ] || [ ! -d "${ascend_include}" ]; then
        echo "========= skip build HYBM OPS: CANN cross-compiler or headers are unavailable ============"
        return 0
    fi

    echo "========= build HYBM OPS ============"
    rm -rf "${ops_build_dir}"
    cmake \
        -S "${PROJ_DIR}/src/hybm/ops" \
        -B "${ops_build_dir}" \
        -DHYBM_KERNEL_PROJECT_ROOT="${PROJ_DIR}" \
        -DTARGET_INSTALL_DIR="${PROJ_DIR}/output" \
        -DPROJECT_HYBM_SRC_BASE="${PROJ_DIR}/src/hybm" \
        -DPROJECT_UTIL_SRC_BASE="${PROJ_DIR}/src/util/csrc" \
        -DASCEND_HOME_PATH="${ascend_root}" \
        -DCMAKE_BUILD_TYPE="${BUILD_MODE}"
    cmake --build "${ops_build_dir}" --target install --parallel "${MF_BUILD_JOBS}"

    local mf_version
    local git_commit
    mf_version="$(tr -d '[:space:]' < "${PROJ_DIR}/VERSION")"
    git_commit="$(git -C "${PROJ_DIR}" rev-parse HEAD 2>/dev/null || true)"
    {
        echo "mf version info:"
        echo "mf version: ${mf_version}"
        echo "git: ${git_commit}"
    } > "${ops_output_dir}/cann_hybm_kernel_version"

    for artifact in cann-hybm-compat.tar.gz libcann_hybm_kernel.json install.sh cann_hybm_kernel_version; do
        if [ ! -f "${ops_output_dir}/${artifact}" ]; then
            echo "Error: missing HYBM OPS artifact: ${ops_output_dir}/${artifact}" >&2
            return 1
        fi
    done
    bash "${PROJ_DIR}/script/signtool/sign.sh" "${mf_version}"
    echo "========= build HYBM OPS done ============"
}

cd ${ROOT_PATH}/..
PROJ_DIR=$(pwd)
LIBBOUNDSCHECK_INSTALL_PATH="${PROJ_DIR}/build/_deps/hcom-src/dist/hcom_3rdparty/libboundscheck/lib/"

if [ "${BUILD_PYTHON}" == "ON" ]; then
    readonly BACK_PATH_EVN=$PATH

    # 如果 PYTHON_HOME 不存在，则设置默认值
    if [ -z "$PYTHON_HOME" ]; then
        # 定义要检查的目录路径
        CHECK_DIR="/usr/local/python3.11"
        # 判断目录是否存在
        if [ -d "$CHECK_DIR" ]; then
            export PYTHON_HOME="$CHECK_DIR"
        else
            export PYTHON_HOME="/usr/local/"
        fi
        echo "Not set PYTHON_HOME, and use $PYTHON_HOME"
    fi

    export LD_LIBRARY_PATH=$PYTHON_HOME/lib:$LD_LIBRARY_PATH
    export PATH=$PYTHON_HOME/bin:$BACK_PATH_EVN
    export CMAKE_PREFIX_PATH=$PYTHON_HOME
    export LD_LIBRARY_PATH="${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/lib":$LD_LIBRARY_PATH # fix `auditwheel repair` failed
fi

if [ "${BUILD_TOOL}" == "cmake" ]; then
    rm -rf ./build ./output
    if command -v ninja &> /dev/null; then
        echo "========= build by ninja ============"
        export GENERATOR="Ninja"
        export MAKE_CMD=ninja
    else
        GENERATOR="Unix Makefiles"
        export MAKE_CMD=make
    fi
    mkdir build/
    cmake \
        -G "$GENERATOR"  \
        -DCMAKE_BUILD_TYPE="${BUILD_MODE}" \
        -DBUILD_UT="${BUILD_UT}" \
        -DBUILD_OPEN_ABI="${BUILD_OPEN_ABI}" \
        -DBUILD_PYTHON="${BUILD_PYTHON}" \
        -DENABLE_PTRACER="${ENABLE_PTRACER}" \
        -DXPU_TYPE="${XPU_TYPE}" \
        -DBUILD_TEST="${BUILD_TEST}" \
        -DBUILD_HCOM="${BUILD_HCOM}" \
        -DBUILD_WITH_RDMA="${BUILD_HCOM_WITH_RDMA}" \
        -DBUILD_WITH_UB="${BUILD_HCOM_WITH_UB}" \
        -DBUILD_ETCD_BACKEND="${BUILD_ETCD_BACKEND}" \
        -S . \
        -B build/
    ${MAKE_CMD} install -j"${MF_BUILD_JOBS}" -C build/
else
    BAZEL_ARGS=()

    if [ "${BUILD_MODE}" == "DEBUG" ]; then
        BAZEL_ARGS+=("--compilation_mode=dbg")
    else
        BAZEL_ARGS+=("--compilation_mode=opt")
    fi

    if [ "${BUILD_UT}" == "ON" ]; then
        BAZEL_ARGS+=("--copt=-DUT_ENABLED")
    fi

    if [ "${BUILD_OPEN_ABI}" == "OFF" ]; then
        BAZEL_ARGS+=("--copt=-D_GLIBCXX_USE_CXX11_ABI=0")
    fi

    if [ "${ENABLE_PTRACER}" == "ON" ]; then
        BAZEL_ARGS+=("--copt=-DENABLE_PTRACER")
    fi

    if [ "${XPU_TYPE}" == "NPU" ]; then
        BAZEL_ARGS+=("--copt=-DASCEND_NPU")
    elif [ "${XPU_TYPE}" == "GPU" ]; then
        BAZEL_ARGS+=("--copt=-DNVIDIA_GPU")
    else
        BAZEL_ARGS+=("--copt=-DNO_XPU")
    fi

    if [ "${BUILD_HCOM}" == "ON" ]; then
        BAZEL_ARGS+=("--define=build_with_hcom=1")

        if [ "${BUILD_HCOM_WITH_RDMA}" == "OFF" ]; then
            BAZEL_ARGS+=("--define=hcom_enable_rdma=0")
        fi
        if [ "${BUILD_HCOM_WITH_UB}" == "ON" ]; then
            BAZEL_ARGS+=("--define=hcom_enable_ub=1")
        fi
    fi

    if [ "${BUILD_ETCD_BACKEND}" == "OFF" ]; then
        BAZEL_ARGS+=("--build_tag_filters=-enable_etcd_client")
    fi

    BAZEL_ARGS+=("--explain=explain.log")
    BAZEL_ARGS+=("--verbose_explanations")

    rm -rf ./output
    rm -rf ./build

    echo "bazel clean --async"
    bazel clean --async

    echo "bazel build arguments: ${BAZEL_ARGS[@]}"
    bazel build //src/... "${BAZEL_ARGS[@]}"
    copy_bazel_artifacts
fi

# hcom
if [ "${BUILD_HCOM}" == "ON" ]; then

    echo "========= copy hcom lib ============"

    # Check if the source code compilation output directory of libboundscheck exists
    if [ ! -d "$LIBBOUNDSCHECK_INSTALL_PATH" ]; then
        mkdir -p "$LIBBOUNDSCHECK_INSTALL_PATH"
    fi

    # Check if libboundscheck.so exists in the compilation output directory; if not, copy it from the system library directories.
    if [ ! -f "$LIBBOUNDSCHECK_INSTALL_PATH/libboundscheck.so" ]; then
        if [ -f "/usr/lib64/libboundscheck.so" ]; then
            cp -v /usr/lib64/libboundscheck.so "$LIBBOUNDSCHECK_INSTALL_PATH"
        elif [ -f "/usr/lib/libboundscheck.so" ]; then
            cp -v /usr/lib/libboundscheck.so "$LIBBOUNDSCHECK_INSTALL_PATH"
        else
            echo "Error: libboundscheck.so not found"
        fi
    fi
fi

build_hybm_ops

if [ "${BUILD_PYTHON}" != "ON" ]; then
    echo "========= skip build python ============"
        cd ${CURRENT_DIR}
        exit 0
fi

# memfabric_hybrid
mkdir -p ${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/lib
# --- 动态库：lib/ ---
\cp -v "${PROJ_DIR}/output/smem/lib64/libmf_smem.so" "${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/lib/"
\cp -v "${PROJ_DIR}/output/hybm/lib64/libmf_hybm_core.so" "${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/lib/"
\cp -v "${PROJ_DIR}/output/acc_offload/lib64/libmf_acc_offload.so" "${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/lib/"
# --- 头文件：smem/include/host/ ---
mkdir -p ${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/include/smem/host
cp -v "${PROJ_DIR}/output/smem/include/host/"*.h "${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/include/smem/host/"
# --- 头文件：smem/include/device/ ---
mkdir -p ${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/include/smem/device
cp -v "${PROJ_DIR}/output/smem/include/device/"*.h "${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/include/smem/device/"
# --- 头文件：hybm/include/ ---
mkdir -p ${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/include/hybm
cp -v "${PROJ_DIR}/output/hybm/include/"*.h "${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/include/hybm/"

if [ "${BUILD_HCOM}" == "ON" ]; then
    echo "========= copy hcom lib to wheel pkg ============"
    cp -v "${PROJ_DIR}"/output/3rdparty/hcom/lib/libhcom.so "${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/lib"
    cp -v "${LIBBOUNDSCHECK_INSTALL_PATH}"/libboundscheck.so \
          "${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/lib"
fi

if [ "${BUILD_ETCD_BACKEND}" == "ON" ]; then
    echo "========= copy etcd client lib to wheel pkg============"
    cp -v ${PROJ_DIR}/output/etcd/lib64/libetcd_client_v3.so "${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/lib"
fi

VERSION="$(cat VERSION | tr -d '[:space:]')"
export MEMFABRIC_VERSION="${VERSION}"
echo "VERSION IS ${VERSION}"
GIT_COMMIT=`git rev-parse HEAD` || true
{
  echo "mf version info:"
  echo "mf version: ${MEMFABRIC_VERSION}"
  echo "git: ${GIT_COMMIT}"
} > "${PROJ_DIR}/output/VERSION"

cp "${PROJ_DIR}/output/VERSION" "${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/"
cp -v "${PROJ_DIR}/script/mem_scan.py" "${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/mem_scan.py"

# Stage the prebuilt OPS payload as regular wheel package data. build.sh
# already stages and cleans other generated wheel assets in the same way.
WHEEL_AICPU_DIR="${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/_aicpu"
rm -rf "${WHEEL_AICPU_DIR}"
OPS_OUTPUT_DIR="${PROJ_DIR}/output/hybm/aicpu_kernel"
OPS_ARTIFACTS=(cann-hybm-compat.tar.gz libcann_hybm_kernel.json cann_hybm_kernel_version install.sh)
OPS_READY=1
for artifact in "${OPS_ARTIFACTS[@]}"; do
    if [ ! -f "${OPS_OUTPUT_DIR}/${artifact}" ]; then
        OPS_READY=0
        break
    fi
done
if [ "${OPS_READY}" -eq 1 ]; then
    mkdir -p "${WHEEL_AICPU_DIR}"
    for artifact in "${OPS_ARTIFACTS[@]}"; do
        cp "${OPS_OUTPUT_DIR}/${artifact}" "${WHEEL_AICPU_DIR}/"
    done
    echo "========= stage HYBM OPS payload for wheel ============"
else
    echo "========= build wheel without HYBM OPS payload: CANN build was skipped ============"
fi

# 如果 PYTHON_HOME 不存在，则设置默认值
if [ -z "$PYTHON_HOME" ]; then
    # 定义要检查的目录路径
    CHECK_DIR="/usr/local/python3.11"
    # 判断目录是否存在
    if [ -d "$CHECK_DIR" ]; then
        export PYTHON_HOME="$CHECK_DIR"
    else
        export PYTHON_HOME="/usr/local/"
    fi
    echo "Not set PYTHON_HOME，and use $PYTHON_HOME"
fi

python_path_list=("/opt/buildtools/python-3.8.5" "/opt/buildtools/python-3.9.11" "/opt/buildtools/python-3.10.2" "/opt/buildtools/python-3.11.4")
for python_path in "${python_path_list[@]}"
do
    if [ -n "${multiple_python}" ]; then
        export PYTHON_HOME=${python_path}
        export CMAKE_PREFIX_PATH=$PYTHON_HOME
        export LD_LIBRARY_PATH=$PYTHON_HOME/lib
        export PATH=$PYTHON_HOME/bin:$BACK_PATH_EVN

        rm -rf build/
        mkdir -p build/
        if [ "${BUILD_TOOL}" == "cmake" ]; then
            cmake -G "$GENERATOR" -DCMAKE_BUILD_TYPE="${BUILD_MODE}" -DBUILD_OPEN_ABI="${BUILD_OPEN_ABI}" -S . -B build/
            ${MAKE_CMD} -j"${MF_BUILD_JOBS}" -C build
        else
            bazel clean --async
            bazel build //src/... "${BAZEL_ARGS[@]}"
            copy_bazel_artifacts
        fi
    fi

    # memfabric_hybrid
    rm -rf "${PROJ_DIR}"/src/smem/python/memfabric_hybrid/memfabric_hybrid/_pymf_hybrid*.so
    \cp -v "${PROJ_DIR}"/build/src/smem/csrc/python_wrapper/memfabric_hybrid/_pymf_hybrid*.so "${PROJ_DIR}"/src/smem/python/memfabric_hybrid/memfabric_hybrid/
    rm -rf "${PROJ_DIR}"/src/smem/python/memfabric_hybrid/memfabric_hybrid/_pymf_transfer*.so
    \cp -v "${PROJ_DIR}"/build/src/smem/csrc/python_wrapper/mk_transfer_adapter/_pymf_transfer*.so "${PROJ_DIR}"/src/smem/python/memfabric_hybrid/memfabric_hybrid/
    rm -rf "${PROJ_DIR}"/src/smem/python/memfabric_hybrid/memfabric_hybrid/_pymf_acc_offload*.so
    \cp -v "${PROJ_DIR}"/build/src/acc_offload/csrc/python_wrapper/_pymf_acc_offload*.so "${PROJ_DIR}"/src/smem/python/memfabric_hybrid/memfabric_hybrid/
    cd "${PROJ_DIR}/src/smem/python/memfabric_hybrid"
    rm -rf build memfabric_hybrid.egg-info
    if check_contains_path "${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/lib"; then
        echo "========= not contain add to LD_LIBRARY_PATH ============"
        export LD_LIBRARY_PATH="${PROJ_DIR}/src/smem/python/memfabric_hybrid/memfabric_hybrid/lib":$LD_LIBRARY_PATH # fix `auditwheel repair` failed
    fi

    python3 setup.py bdist_wheel
    cd "${PROJ_DIR}"

    if [ -z "${multiple_python}" ];then
        break
    fi
done

# memfabric_hybrid
mkdir -p "${PROJ_DIR}/output/memfabric_hybrid/wheel"
cp "${PROJ_DIR}"/src/smem/python/memfabric_hybrid/dist/*.whl "${PROJ_DIR}/output/memfabric_hybrid/wheel"
rm -rf "${PROJ_DIR}"/src/smem/python/memfabric_hybrid/dist
rm -rf "${PROJ_DIR}"/src/smem/python/memfabric_hybrid/memfabric_hybrid/include
rm -rf "${PROJ_DIR}"/src/smem/python/memfabric_hybrid/memfabric_hybrid/lib
rm -rf "${PROJ_DIR}"/src/smem/python/memfabric_hybrid/memfabric_hybrid/script
rm -rf "${PROJ_DIR}"/src/smem/python/memfabric_hybrid/memfabric_hybrid/_aicpu
rm -f "${PROJ_DIR}"/src/smem/python/memfabric_hybrid/memfabric_hybrid/mem_scan.py

cd ${CURRENT_DIR}
