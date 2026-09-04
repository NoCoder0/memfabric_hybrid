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

set -Eeuo pipefail

readonly PACKAGE_NAME="cann-hybm-compat.tar.gz"
readonly JSON_NAME="libcann_hybm_kernel.json"
readonly VERSION_FILE="cann_hybm_kernel_version"
readonly SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
readonly PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../../.." 2>/dev/null && pwd || true)"
readonly LEGACY_STATE_BASENAME=".hybm_aicpu_kernel_state"
readonly LEGACY_BACKUP_BASENAME=".hybm_aicpu_kernel_backup"
readonly MIN_CANN_VERSION="9.1.0"

ACTION=""
INSTALL_FOR_ALL=0
OWNER_ID=""
EXPECTED_OWNER=""
TEMP_FILES=()
LEGACY_KERNEL_DIR_CREATED=0
LEGACY_CONFIG_DIR_CREATED=0

log_error() {
    printf 'ERROR: %s\n' "$*" >&2
}

cleanup_temporary_files() {
    local path
    for path in "${TEMP_FILES[@]}"; do
        rm -f -- "${path}" 2>/dev/null || true
    done
}

handle_error() {
    local exit_code="$1"
    local line="$2"
    log_error "HYBM AICPU ${ACTION:-operation} failed, CANN root=${CANN_ROOT:-unset}, line=${line}, status=${exit_code}"
    exit "${exit_code}"
}

trap cleanup_temporary_files EXIT
trap 'handle_error "$?" "${LINENO}"' ERR

usage() {
    cat <<'EOF'
Usage: install.sh [OPTIONS]

Install, uninstall, or inspect HYBM AICPU kernel files in the CANN OPP tree.
Installation also updates ${ASCEND_HOME_PATH}/conf/ascend_package_load.ini.

Options:
  -h, --help          Show this help message
  --install           Perform installation (default when no option is given)
    --install-for-all Make installed files readable by all users
  --uninstall         Remove MemFabric-owned files and stanza
  --info              Show install paths and the MemFabric CANN load configuration
  --version           Show the installed HYBM AICPU kernel version
EOF
}

find_source_dir() {
    local output_dir="${PROJECT_ROOT}/output/hybm/aicpu_kernel"
    if source_files_exist "${SCRIPT_DIR}"; then
        printf '%s\n' "${SCRIPT_DIR}"
    elif [[ -n "${PROJECT_ROOT}" ]] && source_files_exist "${output_dir}"; then
        printf 'WARNING: using source-tree build output, path=%s\n' "${output_dir}" >&2
        printf '%s\n' "${output_dir}"
    else
        return 0
    fi
}

source_files_exist() {
    local directory="$1"
    [[ -f "${directory}/${PACKAGE_NAME}" && ! -L "${directory}/${PACKAGE_NAME}" ]] \
        && [[ -f "${directory}/${JSON_NAME}" && ! -L "${directory}/${JSON_NAME}" ]] \
        && [[ -f "${directory}/${VERSION_FILE}" && ! -L "${directory}/${VERSION_FILE}" ]]
}

get_cann_version() {
    local cann_root="$1"
    local version_file version
    local candidates=(
        "${cann_root}/$(uname -m)-linux/ascend_toolkit_install.info"
        "${cann_root}/aarch64-linux/ascend_toolkit_install.info"
        "${cann_root}/x86_64-linux/ascend_toolkit_install.info"
        "${cann_root}/ascend_toolkit_install.info"
        "${cann_root}/version.cfg"
    )
    for version_file in "${candidates[@]}"; do
        [[ -f "${version_file}" ]] || continue
        version="$(awk -F= '
            tolower($1) ~ /^[[:space:]]*version[[:space:]]*$/ {
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2); print $2; exit
            }
        ' "${version_file}")"
        if [[ -n "${version}" ]]; then
            printf '%s\n' "${version}"
            return 0
        fi
    done
    return 1
}

is_cann_version_supported() {
    local version="$1"
    [[ "${version}" =~ ^[^0-9]*([0-9]+)\.([0-9]+)(\.([0-9]+))? ]] || return 1
    local major=$((10#${BASH_REMATCH[1]}))
    local minor=$((10#${BASH_REMATCH[2]}))
    local patch=$((10#${BASH_REMATCH[4]:-0}))
    (( major > 9 || (major == 9 && minor > 1) || (major == 9 && minor == 1 && patch >= 0) ))
}

validate_regular_source() {
    local path="$1"
    if [[ -L "${path}" || ! -f "${path}" ]]; then
        log_error "source is missing or is not a regular file, path=${path}"
        return 1
    fi
}

validate_owned_target() {
    local target="$1"
    if [[ -L "${target}" || ( -e "${target}" && ! -f "${target}" ) ]]; then
        log_error "owned target is a symlink or non-regular object, path=${target}"
        return 1
    fi
}

validate_directory_target() {
    local path="$1"
    if [[ -L "${path}" || ( -e "${path}" && ! -d "${path}" ) ]]; then
        log_error "directory target is a symlink or non-directory object, path=${path}"
        return 1
    fi
}

validate_directory_chain() {
    local relative_path="$1"
    local current="${CANN_ROOT}"
    local component
    local resolved
    local components=()
    IFS='/' read -r -a components <<< "${relative_path}"
    for component in "${components[@]}"; do
        [[ -n "${component}" ]] || continue
        current="${current}/${component}"
        if [[ -L "${current}" ]]; then
            resolved="$(readlink -f "${current}" 2>/dev/null || true)"
            if [[ ! -d "${resolved}" || ( "${resolved}" != "${CANN_ROOT}" \
                && "${resolved}" != "${CANN_ROOT}/"* ) ]]; then
                log_error "directory symlink escapes CANN root or is invalid, path=${current}," \
                    "target=${resolved:-unset}"
                return 1
            fi
        elif [[ -e "${current}" && ! -d "${current}" ]]; then
            log_error "directory path contains a non-directory object, path=${current}"
            return 1
        fi
    done
}

validate_cann_paths() {
    validate_directory_chain "conf"
    validate_directory_chain "opp/vendors/cust/op_impl/aicpu/hybm/kernel"
    validate_directory_chain "opp/vendors/cust/op_impl/aicpu/hybm/config"
}

validate_legacy_layout() {
    local path
    validate_directory_chain "opp/vendors/cust/op_impl/aicpu/kernel"
    validate_directory_chain "opp/vendors/cust/op_impl/aicpu/config"
    validate_directory_chain "opp/vendors/cust/${LEGACY_STATE_BASENAME}"
    for path in "${LEGACY_KERNEL_DIR}/${PACKAGE_NAME}" "${LEGACY_CONFIG_DIR}/${JSON_NAME}" \
        "${LEGACY_CONFIG_DIR}/${VERSION_FILE}" "${LEGACY_STATE_FILE}"; do
        validate_owned_target "${path}"
    done
}

validate_shared_ini() {
    if [[ -L "${INI_FILE}" || ( -e "${INI_FILE}" && ! -f "${INI_FILE}" ) ]]; then
        log_error "CANN load configuration is a symlink or non-regular object, path=${INI_FILE}"
        return 1
    fi
}

ini_has_memfabric_stanza() {
    awk -v target="name:${PACKAGE_NAME}" '
        { line = $0; sub(/\r$/, "", line); if (line == target) found = 1 }
        END { exit found ? 0 : 1 }
    ' "$1"
}

validate_state_storage() {
    validate_directory_target "${STATE_DIR}" || return 1
    if [[ -L "${STATE_FILE}" || ( -e "${STATE_FILE}" && ! -f "${STATE_FILE}" ) ]]; then
        log_error "installer state is a symlink or non-regular object, path=${STATE_FILE}"
        return 1
    fi
}

read_state_value() {
    local key="$1"
    read_state_file_value "${STATE_FILE}" "${key}"
}

read_state_file_value() {
    local state_file="$1"
    local key="$2"
    awk -F= -v key="${key}" '$1 == key { print $2; exit }' "${state_file}"
}

load_state() {
    STATE_PRESENT=0
    CURRENT_OWNER_ID=""
    INI_CREATED=0
    CONF_DIR_CREATED=0
    OPP_DIR_CREATED=0
    VENDORS_DIR_CREATED=0
    CUST_DIR_CREATED=0
    OP_IMPL_DIR_CREATED=0
    AICPU_DIR_CREATED=0
    HYBM_DIR_CREATED=0
    KERNEL_DIR_CREATED=0
    CONFIG_DIR_CREATED=0
    LEGACY_KERNEL_DIR_CREATED=0
    LEGACY_CONFIG_DIR_CREATED=0
    [[ -f "${STATE_FILE}" ]] || return 0
    STATE_PRESENT=1
    local status
    status="$(read_state_value status)"
    if [[ "${status}" != "pending" && "${status}" != "installed" ]]; then
        log_error "installer state has invalid status, path=${STATE_FILE}, status=${status:-empty}"
        return 1
    fi
    CURRENT_OWNER_ID="$(read_state_value owner_id)"
    load_created_flag ini_created INI_CREATED
    load_created_flag conf_dir_created CONF_DIR_CREATED
    load_created_flag opp_dir_created OPP_DIR_CREATED
    load_created_flag vendors_dir_created VENDORS_DIR_CREATED
    load_created_flag cust_dir_created CUST_DIR_CREATED
    load_created_flag op_impl_dir_created OP_IMPL_DIR_CREATED
    load_created_flag aicpu_dir_created AICPU_DIR_CREATED
    load_created_flag hybm_dir_created HYBM_DIR_CREATED
    load_created_flag kernel_dir_created KERNEL_DIR_CREATED
    load_created_flag config_dir_created CONFIG_DIR_CREATED
}

load_created_flag() {
    local key="$1"
    local variable="$2"
    load_created_flag_from "${STATE_FILE}" "${key}" "${variable}"
}

load_created_flag_from() {
    local state_file="$1"
    local key="$2"
    local variable="$3"
    local value
    value="$(read_state_file_value "${state_file}" "${key}")"
    if [[ -z "${value}" ]]; then
        return 0
    fi
    if [[ "${value}" != "0" && "${value}" != "1" ]]; then
        log_error "installer state has invalid flag, path=${state_file}, key=${key}, value=${value:-empty}"
        return 1
    fi
    printf -v "${variable}" '%s' "${value}"
}

load_legacy_state() {
    [[ -f "${LEGACY_STATE_FILE}" ]] || return 0
    local status
    status="$(read_state_file_value "${LEGACY_STATE_FILE}" status)"
    if [[ "${status}" != "pending" && "${status}" != "installed" ]]; then
        log_error "legacy installer state has invalid status, path=${LEGACY_STATE_FILE}, status=${status:-empty}"
        return 1
    fi
    load_created_flag_from "${LEGACY_STATE_FILE}" kernel_dir_created LEGACY_KERNEL_DIR_CREATED
    load_created_flag_from "${LEGACY_STATE_FILE}" config_dir_created LEGACY_CONFIG_DIR_CREATED
    [[ ${STATE_PRESENT} -eq 0 ]] || return 0
    STATE_PRESENT=1
    CURRENT_OWNER_ID="$(read_state_file_value "${LEGACY_STATE_FILE}" owner_id)"
    load_legacy_shared_flags
}

load_legacy_shared_flags() {
    load_created_flag_from "${LEGACY_STATE_FILE}" ini_created INI_CREATED
    load_created_flag_from "${LEGACY_STATE_FILE}" conf_dir_created CONF_DIR_CREATED
    load_created_flag_from "${LEGACY_STATE_FILE}" opp_dir_created OPP_DIR_CREATED
    load_created_flag_from "${LEGACY_STATE_FILE}" vendors_dir_created VENDORS_DIR_CREATED
    load_created_flag_from "${LEGACY_STATE_FILE}" cust_dir_created CUST_DIR_CREATED
    load_created_flag_from "${LEGACY_STATE_FILE}" op_impl_dir_created OP_IMPL_DIR_CREATED
    load_created_flag_from "${LEGACY_STATE_FILE}" aicpu_dir_created AICPU_DIR_CREATED
}

remember_created_objects() {
    [[ -e "${INI_FILE}" || -L "${INI_FILE}" ]] || INI_CREATED=1
    [[ -e "${CONF_DIR}" || -L "${CONF_DIR}" ]] || CONF_DIR_CREATED=1
    [[ -e "${OPP_DIR}" || -L "${OPP_DIR}" ]] || OPP_DIR_CREATED=1
    [[ -e "${VENDORS_DIR}" || -L "${VENDORS_DIR}" ]] || VENDORS_DIR_CREATED=1
    [[ -e "${CUST_DIR}" || -L "${CUST_DIR}" ]] || CUST_DIR_CREATED=1
    [[ -e "${OP_IMPL_DIR}" || -L "${OP_IMPL_DIR}" ]] || OP_IMPL_DIR_CREATED=1
    [[ -e "${AICPU_DIR}" || -L "${AICPU_DIR}" ]] || AICPU_DIR_CREATED=1
    [[ -e "${HYBM_DIR}" || -L "${HYBM_DIR}" ]] || HYBM_DIR_CREATED=1
    [[ -e "${KERNEL_DIR}" || -L "${KERNEL_DIR}" ]] || KERNEL_DIR_CREATED=1
    [[ -e "${CONFIG_DIR}" || -L "${CONFIG_DIR}" ]] || CONFIG_DIR_CREATED=1
}

write_state() {
    local status="$1"
    mkdir -p -- "${STATE_DIR}"
    local temporary
    temporary="$(mktemp "${STATE_DIR}/state.XXXXXX")"
    TEMP_FILES+=("${temporary}")
    {
        printf 'status=%s\n' "${status}"
        printf 'owner_id=%s\n' "${OWNER_ID}"
        printf 'ini_created=%s\n' "${INI_CREATED}"
        printf 'conf_dir_created=%s\n' "${CONF_DIR_CREATED}"
        printf 'opp_dir_created=%s\n' "${OPP_DIR_CREATED}"
        printf 'vendors_dir_created=%s\n' "${VENDORS_DIR_CREATED}"
        printf 'cust_dir_created=%s\n' "${CUST_DIR_CREATED}"
        printf 'op_impl_dir_created=%s\n' "${OP_IMPL_DIR_CREATED}"
        printf 'aicpu_dir_created=%s\n' "${AICPU_DIR_CREATED}"
        printf 'hybm_dir_created=%s\n' "${HYBM_DIR_CREATED}"
        printf 'kernel_dir_created=%s\n' "${KERNEL_DIR_CREATED}"
        printf 'config_dir_created=%s\n' "${CONFIG_DIR_CREATED}"
    } > "${temporary}"
    chmod 600 "${temporary}"
    mv -Tf -- "${temporary}" "${STATE_FILE}"
}

preflight_install() {
    local source target
    validate_cann_paths
    validate_legacy_layout
    validate_state_storage
    for source in "${SOURCE_DIR}/${PACKAGE_NAME}" "${SOURCE_DIR}/${JSON_NAME}" \
        "${SOURCE_DIR}/${VERSION_FILE}"; do
        validate_regular_source "${source}"
    done
    for source in "${SOURCE_DIR}/${PACKAGE_NAME}" "${SOURCE_DIR}/${JSON_NAME}" \
        "${SOURCE_DIR}/${VERSION_FILE}"; do
        target="$(target_for_source "${source}")"
        validate_owned_target "${target}"
    done
    validate_shared_ini
}

target_for_source() {
    case "$(basename "$1")" in
        "${PACKAGE_NAME}") printf '%s\n' "${KERNEL_DIR}/${PACKAGE_NAME}" ;;
        "${JSON_NAME}") printf '%s\n' "${CONFIG_DIR}/${JSON_NAME}" ;;
        "${VERSION_FILE}") printf '%s\n' "${CONFIG_DIR}/${VERSION_FILE}" ;;
    esac
}

publish_owned_file() {
    local source="$1"
    local target="$2"
    local permission="$3"
    local temporary
    temporary="$(mktemp "${target}.memfabric.XXXXXX")"
    TEMP_FILES+=("${temporary}")
    cp -- "${source}" "${temporary}"
    chmod "${permission}" "${temporary}"
    mv -Tf -- "${temporary}" "${target}"
    printf 'Installed: %s\n' "${target}"
}

filter_memfabric_stanzas() {
    local source="$1"
    local destination="$2"
    awk -v target="name:${PACKAGE_NAME}" '
        {
            line = $0
            sub(/\r$/, "", line)
            if (line ~ /^name:/) skipping = (line == target)
            if (!skipping) print $0
        }
    ' "${source}" > "${destination}"
}

preserve_ini_metadata() {
    local temporary="$1"
    if [[ -f "${INI_FILE}" ]]; then
        chmod --reference="${INI_FILE}" "${temporary}"
        if [[ "$(stat -c '%u:%g' "${temporary}")" != "$(stat -c '%u:%g' "${INI_FILE}")" ]]; then
            chown --reference="${INI_FILE}" "${temporary}"
        fi
    else
        chmod "$2" "${temporary}"
    fi
}

rewrite_ini() {
    local append_stanza="$1"
    local permission="$2"
    local temporary
    temporary="$(mktemp "${CONF_DIR}/.ascend_package_load.ini.memfabric.XXXXXX")"
    TEMP_FILES+=("${temporary}")
    if [[ -f "${INI_FILE}" ]]; then
        filter_memfabric_stanzas "${INI_FILE}" "${temporary}"
    fi
    if [[ "${append_stanza}" -eq 1 ]]; then
        append_standard_stanza "${temporary}"
    elif [[ "${INI_CREATED}" -eq 1 && ! -s "${temporary}" ]]; then
        rm -f -- "${temporary}" "${INI_FILE}"
        printf 'Removed empty: %s\n' "${INI_FILE}"
        return 0
    fi
    preserve_ini_metadata "${temporary}" "${permission}"
    # This atomic rewrite preserves completed edits. Simultaneous uncoordinated writers are unsupported.
    mv -Tf -- "${temporary}" "${INI_FILE}"
}

append_standard_stanza() {
    local destination="$1"
    if [[ -s "${destination}" ]] \
        && [[ "$(tail -c 1 "${destination}" | od -An -tx1 | tr -d ' \n')" != "0a" ]]; then
        printf '\n' >> "${destination}"
    fi
    cat >> "${destination}" <<'EOF'
name:cann-hybm-compat.tar.gz
install_path:2
optional:true
package_path:opp/vendors/cust/op_impl/aicpu/hybm/kernel
load_as_per_soc:false
EOF
}

do_install() {
    preflight_install
    load_state
    load_legacy_state
    remember_created_objects
    write_state pending
    mkdir -p -- "${CONF_DIR}" "${KERNEL_DIR}" "${CONFIG_DIR}"
    local permission=440
    [[ ${INSTALL_FOR_ALL} -eq 0 ]] || permission=444
    publish_owned_file "${SOURCE_DIR}/${PACKAGE_NAME}" "${KERNEL_DIR}/${PACKAGE_NAME}" "${permission}"
    publish_owned_file "${SOURCE_DIR}/${JSON_NAME}" "${CONFIG_DIR}/${JSON_NAME}" "${permission}"
    publish_owned_file "${SOURCE_DIR}/${VERSION_FILE}" "${CONFIG_DIR}/${VERSION_FILE}" "${permission}"
    rewrite_ini 1 "${permission}"
    printf 'Updated: %s\n' "${INI_FILE}"
    write_state installed
    cleanup_legacy_layout
}

preflight_uninstall() {
    local path
    validate_cann_paths
    validate_legacy_layout
    validate_state_storage
    for path in "${KERNEL_DIR}/${PACKAGE_NAME}" "${CONFIG_DIR}/${JSON_NAME}" \
        "${CONFIG_DIR}/${VERSION_FILE}"; do
        if [[ -L "${path}" || ( -e "${path}" && ! -f "${path}" ) ]]; then
            log_error "refusing to remove a symlink or non-regular object, path=${path}"
            return 1
        fi
    done
    if [[ -L "${INI_FILE}" || ( -e "${INI_FILE}" && ! -f "${INI_FILE}" ) ]]; then
        log_error "CANN load configuration is a symlink or non-regular object, path=${INI_FILE}"
        return 1
    fi
}

remove_owned_files() {
    local path
    for path in "${KERNEL_DIR}/${PACKAGE_NAME}" "${CONFIG_DIR}/${JSON_NAME}" \
        "${CONFIG_DIR}/${VERSION_FILE}"; do
        if [[ -f "${path}" ]]; then
            rm -f -- "${path}"
            printf 'Removed: %s\n' "${path}"
        fi
    done
}

cleanup_legacy_layout() {
    local path ret
    for path in "${LEGACY_KERNEL_DIR}/${PACKAGE_NAME}" "${LEGACY_CONFIG_DIR}/${JSON_NAME}" \
        "${LEGACY_CONFIG_DIR}/${VERSION_FILE}" "${LEGACY_STATE_FILE}"; do
        [[ -f "${path}" ]] || continue
        rm -f -- "${path}" || {
            ret=$?
            log_error "remove legacy HYBM AICPU file failed, path=${path}, ret=${ret}"
            return "${ret}"
        }
        printf 'Removed legacy: %s\n' "${path}"
    done
    rmdir "${LEGACY_STATE_DIR}" 2>/dev/null || true
    [[ ${LEGACY_KERNEL_DIR_CREATED} -eq 0 ]] || rmdir "${LEGACY_KERNEL_DIR}" 2>/dev/null || true
    [[ ${LEGACY_CONFIG_DIR_CREATED} -eq 0 ]] || rmdir "${LEGACY_CONFIG_DIR}" 2>/dev/null || true
}

cleanup_legacy_backup() {
    local backup_dir="${CANN_ROOT}/opp/vendors/cust/${LEGACY_BACKUP_BASENAME}"
    if [[ -L "${backup_dir}" || ( -e "${backup_dir}" && ! -d "${backup_dir}" ) ]]; then
        printf 'WARNING: skip unsafe legacy backup path: %s\n' "${backup_dir}" >&2
        return 0
    fi
    local subdir
    for subdir in kernel config conf; do
        if [[ -L "${backup_dir}/${subdir}" \
            || ( -e "${backup_dir}/${subdir}" && ! -d "${backup_dir}/${subdir}" ) ]]; then
            printf 'WARNING: skip unsafe legacy backup subdirectory: %s\n' "${backup_dir}/${subdir}" >&2
            return 0
        fi
    done
    rm -f -- "${backup_dir}/manifest.txt"
    rm -f -- "${backup_dir}/kernel/${PACKAGE_NAME}"
    rm -f -- "${backup_dir}/config/${JSON_NAME}" "${backup_dir}/config/${VERSION_FILE}"
    rm -f -- "${backup_dir}/conf/ascend_package_load.ini"
    rmdir "${backup_dir}/kernel" "${backup_dir}/config" "${backup_dir}/conf" 2>/dev/null || true
    rmdir "${backup_dir}" 2>/dev/null || true
}

cleanup_created_directories() {
    [[ ${KERNEL_DIR_CREATED} -eq 0 ]] || rmdir "${KERNEL_DIR}" 2>/dev/null || true
    [[ ${CONFIG_DIR_CREATED} -eq 0 ]] || rmdir "${CONFIG_DIR}" 2>/dev/null || true
    [[ ${HYBM_DIR_CREATED} -eq 0 ]] || rmdir "${HYBM_DIR}" 2>/dev/null || true
    [[ ${AICPU_DIR_CREATED} -eq 0 ]] || rmdir "${AICPU_DIR}" 2>/dev/null || true
    [[ ${OP_IMPL_DIR_CREATED} -eq 0 ]] || rmdir "${OP_IMPL_DIR}" 2>/dev/null || true
    [[ ${CUST_DIR_CREATED} -eq 0 ]] || rmdir "${CUST_DIR}" 2>/dev/null || true
    [[ ${VENDORS_DIR_CREATED} -eq 0 ]] || rmdir "${VENDORS_DIR}" 2>/dev/null || true
    [[ ${OPP_DIR_CREATED} -eq 0 ]] || rmdir "${OPP_DIR}" 2>/dev/null || true
    [[ ${CONF_DIR_CREATED} -eq 0 ]] || rmdir "${CONF_DIR}" 2>/dev/null || true
}

owner_matches_expected() {
    if [[ -z "${EXPECTED_OWNER}" ]]; then
        return 0
    fi
    if [[ ${STATE_PRESENT} -eq 1 && "${CURRENT_OWNER_ID}" == "${EXPECTED_OWNER}" ]]; then
        return 0
    fi
    printf 'Skip HYBM OPS uninstall: owner changed, expected=%s, current=%s\n' \
        "${EXPECTED_OWNER}" "${CURRENT_OWNER_ID:-none}"
    return 1
}

cleanup_shared_ini() {
    local ret
    [[ -f "${INI_FILE}" ]] || return 0
    if ini_has_memfabric_stanza "${INI_FILE}"; then
        rewrite_ini 0 440
        printf 'Cleaned: %s\n' "${INI_FILE}"
        return 0
    fi
    if [[ ${INI_CREATED} -eq 0 || -s "${INI_FILE}" ]]; then
        return 0
    fi
    rm -f -- "${INI_FILE}" || {
        ret=$?
        log_error "remove MemFabric-created empty CANN load configuration failed, path=${INI_FILE}, ret=${ret}"
        return "${ret}"
    }
    printf 'Removed empty: %s\n' "${INI_FILE}"
}

do_uninstall() {
    preflight_uninstall
    load_state
    load_legacy_state
    if ! owner_matches_expected; then
        return 0
    fi
    remove_owned_files
    cleanup_legacy_layout
    cleanup_shared_ini
    rm -f -- "${STATE_FILE}"
    cleanup_legacy_backup
    cleanup_created_directories
}

show_info() {
    local path
    printf 'MemFabric Hybrid AICPU paths:\n'
    for path in "${INI_FILE}" "${KERNEL_DIR}/${PACKAGE_NAME}" \
        "${CONFIG_DIR}/${JSON_NAME}" "${CONFIG_DIR}/${VERSION_FILE}" "${STATE_FILE}"; do
        if [[ -e "${path}" || -L "${path}" ]]; then
            printf '  [exists] %s\n' "${path}"
        else
            printf '  [missing] %s\n' "${path}"
        fi
    done
    if [[ -f "${STATE_FILE}" ]]; then
        load_state
        printf '  state: %s\n' "$(read_state_value status)"
    fi
    printf '\nMemFabric Hybrid entry in ascend_package_load.ini:\n'
    if [[ ! -f "${INI_FILE}" ]]; then
        printf '  not configured (%s does not exist)\n' "${INI_FILE}"
        return 0
    fi
    local entry
    entry="$(awk -v target="name:${PACKAGE_NAME}" '
        { line = $0; sub(/\r$/, "", line) }
        line == target { printing = 1 }
        printing && line != target && line ~ /^name:/ { exit }
        printing { print line }
    ' "${INI_FILE}")"
    [[ -z "${entry}" ]] && printf '  not configured\n' || printf '%s\n' "${entry}" | sed 's/^/  /'
}

show_version() {
    local version_path="${CONFIG_DIR}/${VERSION_FILE}"
    local ret
    if [[ -L "${version_path}" || ! -f "${version_path}" ]]; then
        log_error "AICPU kernel version file is missing or is not a regular file, path=${version_path}"
        return 1
    fi
    if [[ ! -r "${version_path}" ]]; then
        log_error "AICPU kernel version file is not readable, path=${version_path}"
        return 1
    fi
    cat -- "${version_path}" || {
        ret=$?
        log_error "read AICPU kernel version file failed, path=${version_path}, ret=${ret}"
        return "${ret}"
    }
}

parse_arguments() {
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --install) set_action install; shift ;;
            --uninstall) set_action uninstall; shift ;;
            --info) set_action info; shift ;;
            --version) set_action version; shift ;;
            --install-for-all) INSTALL_FOR_ALL=1; shift ;;
            --owner-id=*) OWNER_ID="${1#*=}"; shift ;;
            --expected-owner=*) EXPECTED_OWNER="${1#*=}"; shift ;;
            -h|--help) usage; exit 0 ;;
            *) log_error "unknown option, value=$1"; usage >&2; exit 1 ;;
        esac
    done
    if [[ -z "${ACTION}" ]]; then
        if [[ ${INSTALL_FOR_ALL} -eq 1 ]]; then
            log_error "--install-for-all must be used with --install"
            exit 1
        fi
        ACTION=install
    fi
    validate_action_options
}

set_action() {
    local requested="$1"
    if [[ -n "${ACTION}" ]]; then
        log_error "conflicting actions, current=${ACTION}, requested=${requested}"
        exit 1
    fi
    ACTION="${requested}"
}

validate_action_options() {
    if [[ ${INSTALL_FOR_ALL} -eq 1 && "${ACTION}" != install ]]; then
        log_error "--install-for-all must be used with --install"
        exit 1
    fi
    if [[ -n "${OWNER_ID}" && "${ACTION}" != install ]]; then
        log_error "--owner-id must be used with --install"
        exit 1
    fi
    if [[ -n "${EXPECTED_OWNER}" && "${ACTION}" != uninstall ]]; then
        log_error "--expected-owner must be used with --uninstall"
        exit 1
    fi
    validate_owner_id "${OWNER_ID}" "--owner-id"
    validate_owner_id "${EXPECTED_OWNER}" "--expected-owner"
}

validate_owner_id() {
    local value="$1"
    local option="$2"
    if [[ -n "${value}" && ! "${value}" =~ ^[A-Za-z0-9._:-]+$ ]]; then
        log_error "invalid owner identifier, option=${option}, value=${value}"
        exit 1
    fi
}

initialize_paths() {
    if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
        log_error "ASCEND_HOME_PATH is not set"
        exit 1
    fi
    if [[ ! -d "${ASCEND_HOME_PATH}" ]]; then
        log_error "CANN root does not exist, path=${ASCEND_HOME_PATH}"
        exit 1
    fi
    CANN_ROOT="$(cd "${ASCEND_HOME_PATH}" && pwd -P)"
    CONF_DIR="${CANN_ROOT}/conf"
    INI_FILE="${CONF_DIR}/ascend_package_load.ini"
    OPP_DIR="${CANN_ROOT}/opp"
    VENDORS_DIR="${OPP_DIR}/vendors"
    CUST_DIR="${VENDORS_DIR}/cust"
    OP_IMPL_DIR="${CUST_DIR}/op_impl"
    AICPU_DIR="${OP_IMPL_DIR}/aicpu"
    HYBM_DIR="${AICPU_DIR}/hybm"
    KERNEL_DIR="${HYBM_DIR}/kernel"
    CONFIG_DIR="${HYBM_DIR}/config"
    STATE_DIR="${CONFIG_DIR}"
    STATE_FILE="${CONFIG_DIR}/.state"
    LEGACY_KERNEL_DIR="${AICPU_DIR}/kernel"
    LEGACY_CONFIG_DIR="${AICPU_DIR}/config"
    LEGACY_STATE_DIR="${CUST_DIR}/${LEGACY_STATE_BASENAME}"
    LEGACY_STATE_FILE="${LEGACY_STATE_DIR}/state"
}

main() {
    parse_arguments "$@"
    initialize_paths
    if [[ "${ACTION}" == install ]]; then
        if [[ -z "${OWNER_ID}" ]]; then
            OWNER_ID="mfcli-$(date +%s)-$$-${RANDOM}"
        fi
        SOURCE_DIR="$(find_source_dir)"
        if [[ -z "${SOURCE_DIR}" ]]; then
            log_error "kernel payload is incomplete, search path=${SCRIPT_DIR}, project root=${PROJECT_ROOT}"
            exit 1
        fi
        local cann_version
        if ! cann_version="$(get_cann_version "${CANN_ROOT}")"; then
            log_error "cannot detect CANN version, root=${CANN_ROOT}, required>=${MIN_CANN_VERSION}"
            exit 1
        fi
        if ! is_cann_version_supported "${cann_version}"; then
            log_error "unsupported CANN version=${cann_version}, root=${CANN_ROOT}, required>=${MIN_CANN_VERSION}"
            exit 1
        fi
        printf 'Detected supported CANN version: %s\n' "${cann_version}"
        do_install
    elif [[ "${ACTION}" == uninstall ]]; then
        do_uninstall
    elif [[ "${ACTION}" == info ]]; then
        show_info
    else
        show_version
    fi
}

main "$@"
