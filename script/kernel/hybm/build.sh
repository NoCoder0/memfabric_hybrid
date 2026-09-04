#!/usr/bin/env bash
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

readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
readonly OPS_BUILD_DIR="${PROJECT_ROOT}/build/hybm_ops"
readonly OPS_STAGE_ROOT="${PROJECT_ROOT}/build/hybm_ops_stage"
readonly OPS_STAGE_DIR="${OPS_STAGE_ROOT}/hybm/aicpu_kernel"
readonly OPS_OUTPUT_DIR="${PROJECT_ROOT}/output/hybm/aicpu_kernel"
readonly OPS_PUBLISH_DIR="${OPS_OUTPUT_DIR}.new.$$"
readonly MIN_CANN_VERSION="9.1.0"
readonly BUILD_MODE="${BUILD_MODE:-RELEASE}"
readonly MF_BUILD_JOBS="${MF_BUILD_JOBS:-32}"
readonly CONSUMER_FILES=(
    cann-hybm-compat.tar.gz
    libcann_hybm_kernel.json
    cann_hybm_kernel_version
    install.sh
)

log_error()
{
    echo "[ERROR] $*" >&2
}

run_or_error()
{
    local context="$1"
    shift
    "$@" || {
        local ret=$?
        log_error "${context}, ret=${ret}"
        return "${ret}"
    }
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

prepare_clean_state()
{
    run_or_error \
        "clean HYBM OPS directories failed, build=${OPS_BUILD_DIR}, output=${OPS_OUTPUT_DIR}" \
        rm -rf "${OPS_BUILD_DIR}" "${OPS_STAGE_ROOT}" "${OPS_OUTPUT_DIR}" "${OPS_PUBLISH_DIR}"
}

check_build_environment()
{
    local ascend_root="$1"
    local compiler="${ascend_root}/toolkit/toolchain/hcc/bin/aarch64-target-linux-gnu-g++"
    local include_dir="${ascend_root}/pkg_inc/base"
    if [ -z "${ascend_root}" ] || [ ! -d "${ascend_root}" ]; then
        echo "========= skip build HYBM OPS: CANN environment is unavailable ============"
        return 1
    fi
    if [ ! -x "${compiler}" ]; then
        echo "========= skip build HYBM OPS: compiler is unavailable: ${compiler} ============"
        return 1
    fi
    if [ ! -d "${include_dir}" ]; then
        echo "========= skip build HYBM OPS: headers are unavailable: ${include_dir} ============"
        return 1
    fi
    return 0
}

configure_and_build()
{
    local ascend_root="$1"
    cmake \
        -S "${PROJECT_ROOT}/src/hybm/ops" \
        -B "${OPS_BUILD_DIR}" \
        -DHYBM_KERNEL_PROJECT_ROOT="${PROJECT_ROOT}" \
        -DTARGET_INSTALL_DIR="${OPS_STAGE_ROOT}" \
        -DPROJECT_HYBM_SRC_BASE="${PROJECT_ROOT}/src/hybm" \
        -DPROJECT_UTIL_SRC_BASE="${PROJECT_ROOT}/src/util/csrc" \
        -DASCEND_HOME_PATH="${ascend_root}" \
        -DCMAKE_BUILD_TYPE="${BUILD_MODE}" || {
        local ret=$?
        log_error "configure HYBM OPS failed, source=${PROJECT_ROOT}/src/hybm/ops, ret=${ret}"
        return "${ret}"
    }
    cmake --build "${OPS_BUILD_DIR}" --target install --parallel "${MF_BUILD_JOBS}" || {
        local ret=$?
        log_error "build HYBM OPS failed, build_dir=${OPS_BUILD_DIR}, jobs=${MF_BUILD_JOBS}, ret=${ret}"
        return "${ret}"
    }
}

write_version_file()
{
    local version_source="${PROJECT_ROOT}/VERSION"
    local output_file="${OPS_STAGE_DIR}/cann_hybm_kernel_version"
    local mf_version
    local git_commit
    local build_time
    if [ ! -f "${version_source}" ]; then
        log_error "HYBM OPS version source is missing, path=${version_source}"
        return 1
    fi
    mf_version="$(tr -d '[:space:]' < "${version_source}")" || {
        local ret=$?
        log_error "read HYBM OPS version failed, path=${version_source}, ret=${ret}"
        return "${ret}"
    }
    if [ -n "${MEMFABRIC_VERSION_FILE:-}" ]; then
        if [ ! -f "${MEMFABRIC_VERSION_FILE}" ]; then
            log_error "MemFabric version file is missing, path=${MEMFABRIC_VERSION_FILE}"
            return 1
        fi
        cp "${MEMFABRIC_VERSION_FILE}" "${output_file}" || {
            local ret=$?
            log_error \
                "copy MemFabric version file failed, source=${MEMFABRIC_VERSION_FILE}," \
                "target=${output_file}, ret=${ret}"
            return "${ret}"
        }
        echo "${mf_version}"
        return 0
    fi
    git_commit="$(git -C "${PROJECT_ROOT}" rev-parse HEAD 2>/dev/null || true)"
    build_time="$(TZ=Asia/Shanghai date '+%Y-%m-%d %H:%M:%S %Z')"
    {
        echo "mf version info:"
        echo "mf version: ${mf_version}"
        echo "git: ${git_commit}"
        echo "build time: ${build_time}"
    } > "${output_file}" || {
        local ret=$?
        log_error "write HYBM OPS version failed, path=${output_file}, ret=${ret}"
        return "${ret}"
    }
    echo "${mf_version}"
}

sign_staged_package()
{
    local mf_version="$1"
    local package="${OPS_STAGE_DIR}/cann-hybm-compat.tar.gz"
    SIGN_FILE="${package}" bash "${PROJECT_ROOT}/script/signtool/sign.sh" "${mf_version}" || {
        local ret=$?
        log_error "sign HYBM OPS failed, package=${package}, ret=${ret}"
        return "${ret}"
    }
}

stage_consumer_files()
{
    local artifact
    local file_count
    local ret
    file_count="$(find "${OPS_STAGE_DIR}" -mindepth 1 -maxdepth 1 -type f | wc -l)" || {
        ret=$?
        log_error "count HYBM OPS staging files failed, path=${OPS_STAGE_DIR}, ret=${ret}"
        return "${ret}"
    }
    if [ "${file_count}" -ne "${#CONSUMER_FILES[@]}" ]; then
        log_error "unexpected HYBM OPS staging file count, path=${OPS_STAGE_DIR}, count=${file_count}"
        return 1
    fi
    run_or_error "create HYBM OPS publish staging failed, path=${OPS_PUBLISH_DIR}" mkdir -p "${OPS_PUBLISH_DIR}"
    for artifact in "${CONSUMER_FILES[@]}"; do
        if [ ! -s "${OPS_STAGE_DIR}/${artifact}" ]; then
            log_error "HYBM OPS consumer artifact is missing or empty, path=${OPS_STAGE_DIR}/${artifact}"
            return 1
        fi
        cp -p "${OPS_STAGE_DIR}/${artifact}" "${OPS_PUBLISH_DIR}/${artifact}" || {
            local ret=$?
            log_error "stage HYBM OPS artifact failed, artifact=${artifact}, ret=${ret}"
            return "${ret}"
        }
    done
}

publish_consumer_files()
{
    local file_count
    local ret
    file_count="$(find "${OPS_PUBLISH_DIR}" -mindepth 1 -maxdepth 1 -type f | wc -l)" || {
        ret=$?
        log_error "count HYBM OPS publish files failed, path=${OPS_PUBLISH_DIR}, ret=${ret}"
        return "${ret}"
    }
    if [ "${file_count}" -ne "${#CONSUMER_FILES[@]}" ]; then
        log_error "unexpected HYBM OPS publish file count, path=${OPS_PUBLISH_DIR}, count=${file_count}"
        return 1
    fi
    run_or_error \
        "create HYBM OPS output parent failed, path=$(dirname "${OPS_OUTPUT_DIR}")" \
        mkdir -p "$(dirname "${OPS_OUTPUT_DIR}")"
    mv -T "${OPS_PUBLISH_DIR}" "${OPS_OUTPUT_DIR}" || {
        local ret=$?
        log_error "publish HYBM OPS failed, target=${OPS_OUTPUT_DIR}, ret=${ret}"
        return "${ret}"
    }
}

main()
{
    local ascend_root="${ASCEND_HOME_PATH:-}"
    local cann_version
    local mf_version
    prepare_clean_state
    if ! check_build_environment "${ascend_root}"; then
        return 0
    fi
    if ! cann_version="$(get_cann_version "${ascend_root}")"; then
        echo "========= skip build HYBM OPS: cannot detect CANN version (required >= ${MIN_CANN_VERSION}) ============"
        return 0
    fi
    if ! is_cann_version_supported "${cann_version}"; then
        echo "========= skip build HYBM OPS: CANN ${cann_version} does not meet >= ${MIN_CANN_VERSION} ============"
        return 0
    fi
    echo "========= detected CANN ${cann_version} ============"
    echo "========= build HYBM OPS ============"
    configure_and_build "${ascend_root}"
    mf_version="$(write_version_file)"
    sign_staged_package "${mf_version}"
    stage_consumer_files
    publish_consumer_files
    echo "========= build HYBM OPS done ============"
}

trap 'rm -rf "${OPS_PUBLISH_DIR}"' EXIT
main "$@"
