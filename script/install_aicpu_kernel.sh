#!/usr/bin/env bash
set -euo pipefail

PACKAGE_NAME="cann-hybm-compat.tar.gz"
JSON_NAME="libcann_hybm_kernel.json"
VERSION_FILE="cann_hybm_kernel_version"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/.." 2>/dev/null && pwd || true)"
BACKUP_BASENAME=".hybm_aicpu_kernel_backup"
MIN_CANN_VERSION="9.1.0"

ACTION=""
FORCE=0
INSTALL_FOR_ALL=0

usage() {
    cat <<'EOF'
Usage: install.sh [OPTIONS]

Install, uninstall, or inspect HYBM AICPU kernel installation paths in the CANN OPP tree.
Installation also updates ${ASCEND_HOME_PATH}/conf/ascend_package_load.ini.

Options:
  -h, --help          Show this help message
  --noexec            (Handled by .run wrapper) Must be used with --extract=<path>; extract only without installing
  --extract=<path>    (Handled by .run wrapper) Extract payload to <path>
  --install           Perform installation (default when no option given)
    --install-for-all (Must be used with --install) Make files readable by all
                      users (directories 755, files 444)
    --force           (Must be used with --install) Overwrite existing installed files
  --uninstall         Remove installed files and restore backups
  --info              Show install paths and the MemFabric Hybrid CANN load configuration
EOF
}

# ---------------------------------------------------------------
# Locate source directory where payload files reside
# ---------------------------------------------------------------
find_source_dir() {
    # Primary: run package scenario -- payload files co-located with this script
    if [[ -f "${SCRIPT_DIR}/${PACKAGE_NAME}" && -f "${SCRIPT_DIR}/${JSON_NAME}" && -f "${SCRIPT_DIR}/${VERSION_FILE}" ]]; then
        echo "${SCRIPT_DIR}"
    # Fallback: source-tree debugging scenario.
    elif [[ -n "${PROJECT_ROOT:-}" ]] \
        && [[ -f "${PROJECT_ROOT}/output/hybm/aicpu_kernel/${PACKAGE_NAME}" \
        && -f "${PROJECT_ROOT}/output/hybm/aicpu_kernel/${JSON_NAME}" \
        && -f "${PROJECT_ROOT}/output/hybm/aicpu_kernel/${VERSION_FILE}" ]]; then
        echo "WARNING: Running from source tree, using build output at ${PROJECT_ROOT}/output/hybm/aicpu_kernel" >&2
        echo "${PROJECT_ROOT}/output/hybm/aicpu_kernel"
    else
        echo ""
    fi
}

get_cann_version() {
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
        if [[ ! -f "${version_file}" ]]; then
            continue
        fi
        version="$(awk -F= '
            tolower($1) ~ /^[[:space:]]*version[[:space:]]*$/ {
                gsub(/^[[:space:]]+|[[:space:]]+$/, "", $2)
                print $2
                exit
            }
        ' "${version_file}")"
        if [[ -n "${version}" ]]; then
            echo "${version}"
            return 0
        fi
    done
    return 1
}

show_info() {
    local cann_root="$1"
    local ini_file="${cann_root}/conf/ascend_package_load.ini"
    local tar_file="${cann_root}/opp/vendors/cust/op_impl/aicpu/kernel/${PACKAGE_NAME}"
    local json_file="${cann_root}/opp/vendors/cust/op_impl/aicpu/config/${JSON_NAME}"
    local path

    echo "MemFabric Hybrid OPS paths:"
    for path in "$ini_file" "$tar_file" "$json_file"; do
        if [[ -e "$path" ]]; then
            echo "  [exists] $path"
        else
            echo "  [missing] $path"
        fi
    done

    echo ""
    echo "MemFabric Hybrid entry in ascend_package_load.ini:"
    if [[ ! -f "$ini_file" ]]; then
        echo "  not configured (${ini_file} does not exist)"
        return 0
    fi

    local entry
    entry="$(awk -v target="name:${PACKAGE_NAME}" '
        {
            line = $0
            sub(/\r$/, "", line)
        }
        line == target {
            printing = 1
        }
        printing && line != target && line ~ /^name:/ {
            exit
        }
        printing {
            print line
        }
    ' "$ini_file")"
    if [[ -n "$entry" ]]; then
        printf '%s\n' "$entry" | sed 's/^/  /'
    else
        echo "  not configured"
    fi
}

is_cann_version_supported() {
    local version="$1"
    if [[ ! "${version}" =~ ^[^0-9]*([0-9]+)\.([0-9]+)(\.([0-9]+))? ]]; then
        return 1
    fi
    local major=$((10#${BASH_REMATCH[1]}))
    local minor=$((10#${BASH_REMATCH[2]}))
    local patch=$((10#${BASH_REMATCH[4]:-0}))
    (( major > 9 || (major == 9 && minor > 1) || (major == 9 && minor == 1 && patch >= 0) ))
}

# ---------------------------------------------------------------
# --install (and --install-for-all) : copy files to target
# ---------------------------------------------------------------
do_install() {
    local src_dir="$1"
    local kern_dir="$2"
    local config_dir="$3"
    local backup_dir="$4"

    mkdir -p "${kern_dir}" "${config_dir}"

    local pkg_target="${kern_dir}/${PACKAGE_NAME}"
    local json_target="${config_dir}/${JSON_NAME}"
    local ver_target="${config_dir}/${VERSION_FILE}"

    local existing=0
    [[ -f "$pkg_target" ]] && existing=1
    [[ -f "$json_target" ]] && existing=1
    [[ -f "$ver_target" ]] && existing=1

    if [[ $existing -eq 1 && $FORCE -eq 0 ]]; then
        echo "Error: target files already exist. Use --force to overwrite, or --uninstall first." >&2
        exit 1
    fi

    # Backup originals once (only if manifest does not exist yet)
    local manifest="${backup_dir}/manifest.txt"
    if [[ ! -f "$manifest" ]]; then
        local INI_FILE="${ASCEND_HOME_PATH}/conf/ascend_package_load.ini"
        # Backup ini file before modification (if it exists)
        if [[ -f "$INI_FILE" ]]; then
            mkdir -p "${backup_dir}/conf"
            cp -a "$INI_FILE" "${backup_dir}/conf/"
            echo "ascend_package_load.ini" >> "$manifest"
            echo "Backed up: ${INI_FILE}"
        fi
        # Backup existing target files
        if [[ $existing -eq 1 ]]; then
            mkdir -p "${backup_dir}/kernel" "${backup_dir}/config"
            if [[ -f "$pkg_target" ]]; then
                cp -a "$pkg_target" "${backup_dir}/kernel/"
                echo "${PACKAGE_NAME}" >> "$manifest"
                echo "Backed up: ${pkg_target}"
            fi
            if [[ -f "$json_target" ]]; then
                cp -a "$json_target" "${backup_dir}/config/"
                echo "${JSON_NAME}" >> "$manifest"
                echo "Backed up: ${json_target}"
            fi
            if [[ -f "$ver_target" ]]; then
                cp -a "$ver_target" "${backup_dir}/config/"
                echo "${VERSION_FILE}" >> "$manifest"
                echo "Backed up: ${ver_target}"
            fi
        fi
    fi

    cp -f "${src_dir}/${PACKAGE_NAME}" "$pkg_target"
    cp -f "${src_dir}/${JSON_NAME}" "$json_target"
    cp -f "${src_dir}/${VERSION_FILE}" "$ver_target"

    echo "Installed: ${pkg_target}"
    echo "Installed: ${json_target}"
    echo "Installed: ${ver_target}"

    # Write/update ascend_package_load.ini
    local INI_FILE="${ASCEND_HOME_PATH}/conf/ascend_package_load.ini"
    mkdir -p "$(dirname "$INI_FILE")"
    # Remove any existing block for our package
    while grep -q '^name:cann-hybm-compat.tar.gz$' "$INI_FILE" 2>/dev/null; do
        sed -i '/^name:cann-hybm-compat.tar.gz$/,+4d' "$INI_FILE"
    done
    # Preserve existing content: start a new line if the file has no trailing newline.
    if [[ -s "$INI_FILE" ]] && [[ "$(tail -c 1 "$INI_FILE" | od -An -tx1 | tr -d ' \n')" != "0a" ]]; then
        printf '\n' >> "$INI_FILE"
    fi
    # Append our 5-line config (no extra fields)
    cat >> "$INI_FILE" <<'INIEOF'
name:cann-hybm-compat.tar.gz
install_path:2
optional:true
package_path:opp/vendors/cust/op_impl/aicpu/kernel
load_as_per_soc:false
INIEOF
    echo "Updated: ${INI_FILE}"

    # Set appropriate file and directory permissions
    local file_perm=440
    local dir_perm=750
    if [[ $INSTALL_FOR_ALL -eq 1 ]]; then
        file_perm=444
        dir_perm=755
    fi
    chmod "$file_perm" "$pkg_target" "$json_target" "$ver_target"
    chmod "$dir_perm" "${kern_dir}" "${config_dir}"
}

# ---------------------------------------------------------------
# --uninstall : remove installed files and restore backups
# ---------------------------------------------------------------
do_uninstall() {
    local kern_dir="$1"
    local config_dir="$2"
    local backup_dir="$3"

    local pkg_target="${kern_dir}/${PACKAGE_NAME}"
    local json_target="${config_dir}/${JSON_NAME}"
    local ver_target="${config_dir}/${VERSION_FILE}"
    local manifest="${backup_dir}/manifest.txt"

    local removed=0

    if [[ -f "$pkg_target" ]]; then
        rm -f "$pkg_target"
        echo "Removed: ${pkg_target}"
        removed=1
    fi
    if [[ -f "$json_target" ]]; then
        rm -f "$json_target"
        echo "Removed: ${json_target}"
        removed=1
    fi
    if [[ -f "$ver_target" ]]; then
        rm -f "$ver_target"
        echo "Removed: ${ver_target}"
        removed=1
    fi

    local INI_FILE="${ASCEND_HOME_PATH}/conf/ascend_package_load.ini"

    if [[ -f "$manifest" ]]; then
        echo "Restoring from backup..."
        while IFS= read -r fname; do
            if [[ "$fname" == "${PACKAGE_NAME}" ]]; then
                local bkp="${backup_dir}/kernel/${fname}"
                if [[ -f "$bkp" ]]; then
                    cp -a "$bkp" "$pkg_target"
                    echo "Restored: ${pkg_target}"
                    removed=1
                fi
            elif [[ "$fname" == "${JSON_NAME}" ]]; then
                local bkp="${backup_dir}/config/${fname}"
                if [[ -f "$bkp" ]]; then
                    cp -a "$bkp" "$json_target"
                    echo "Restored: ${json_target}"
                    removed=1
                fi
            elif [[ "$fname" == "${VERSION_FILE}" ]]; then
                local bkp="${backup_dir}/config/${fname}"
                if [[ -f "$bkp" ]]; then
                    cp -a "$bkp" "$ver_target"
                    echo "Restored: ${ver_target}"
                    removed=1
                fi
            elif [[ "$fname" == "ascend_package_load.ini" ]]; then
                local bkp="${backup_dir}/conf/${fname}"
                if [[ -f "$bkp" ]]; then
                    mkdir -p "$(dirname "$INI_FILE")"
                    cp -a "$bkp" "$INI_FILE"
                    echo "Restored: ${INI_FILE}"
                    removed=1
                fi
            fi
        done < "$manifest"
        rm -f "$manifest"
    fi

    # If ini still has our block (no backup existed), remove it
    if [[ -f "$INI_FILE" ]] && grep -q '^name:cann-hybm-compat.tar.gz$' "$INI_FILE" 2>/dev/null; then
        while grep -q '^name:cann-hybm-compat.tar.gz$' "$INI_FILE" 2>/dev/null; do
            sed -i '/^name:cann-hybm-compat.tar.gz$/,+4d' "$INI_FILE"
        done
        if [[ -s "$INI_FILE" ]]; then
            echo "Cleaned: ${INI_FILE}"
        else
            rm -f "$INI_FILE"
            echo "Removed empty: ${INI_FILE}"
        fi
        removed=1
    fi

    # Clean up empty backup dirs / stale backup files
    rm -f "${backup_dir}/kernel/${PACKAGE_NAME}" "${backup_dir}/config/${JSON_NAME}" "${backup_dir}/config/${VERSION_FILE}" "${backup_dir}/conf/ascend_package_load.ini" 2>/dev/null || true
    rmdir "${backup_dir}/kernel" "${backup_dir}/config" "${backup_dir}/conf" "${backup_dir}" 2>/dev/null || true

    if [[ $removed -eq 0 ]]; then
        echo "Nothing to uninstall."
    fi
}

# ===============================================================
# Main
# ===============================================================

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --install)
            if [[ -n "$ACTION" && "$ACTION" != "install" ]]; then
                echo "Error: conflicting actions: --${ACTION} and --install" >&2
                usage >&2
                exit 1
            fi
            ACTION="install"; shift ;;
        --install-for-all) INSTALL_FOR_ALL=1; shift ;;
        --force) FORCE=1; shift ;;
        --uninstall)
            if [[ -n "$ACTION" ]]; then
                echo "Error: conflicting actions: --${ACTION} and --uninstall" >&2
                usage >&2
                exit 1
            fi
            ACTION="uninstall"; shift ;;
        --info)
            if [[ -n "$ACTION" ]]; then
                echo "Error: conflicting actions: --${ACTION} and --info" >&2
                usage >&2
                exit 1
            fi
            ACTION="info"; shift ;;
        -h|--help) usage; exit 0 ;;
        --extract=*|--noexec) shift ;;  # handled by .run wrapper
        *)
            echo "Unknown option: $1" >&2
            usage >&2
            exit 1
            ;;
    esac
done

# Default action: install when no option given.
# But --install-for-all and --force are not standalone actions.
if [[ -z "$ACTION" ]]; then
    if [[ $INSTALL_FOR_ALL -eq 1 || $FORCE -eq 1 ]]; then
        echo "Error: --install-for-all and --force must be used with --install" >&2
        usage >&2
        exit 1
    fi
    ACTION="install"
fi

# Ensure --install-for-all and --force are only used with --install
if [[ $INSTALL_FOR_ALL -eq 1 && "$ACTION" != "install" ]]; then
    echo "Error: --install-for-all must be used with --install" >&2
    usage >&2
    exit 1
fi
if [[ $FORCE -eq 1 && "$ACTION" != "install" ]]; then
    echo "Error: --force must be used with --install" >&2
    usage >&2
    exit 1
fi

# Locate payload only when installation needs to copy it. Info and uninstall
# operate on the existing CANN tree and must remain available without payload.
SOURCE_DIR=""
if [[ "$ACTION" == "install" ]]; then
    SOURCE_DIR="$(find_source_dir)"
    if [[ -z "$SOURCE_DIR" ]]; then
        echo "Error: Cannot locate ${PACKAGE_NAME}, ${JSON_NAME}, and ${VERSION_FILE}. Ensure they are in the same directory as this script." >&2
        exit 1
    fi
fi

case "$ACTION" in
    install|uninstall)
        if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
            echo "Error: ASCEND_HOME_PATH is not set. Set it to the CANN root directory." >&2
            exit 1
        fi
        if [[ ! -d "$ASCEND_HOME_PATH" ]]; then
            echo "Error: ASCEND_HOME_PATH=${ASCEND_HOME_PATH} does not exist." >&2
            exit 1
        fi
        CANN_ROOT="${ASCEND_HOME_PATH%/}"
        KERNEL_DIR="${CANN_ROOT}/opp/vendors/cust/op_impl/aicpu/kernel"
        CONFIG_DIR="${CANN_ROOT}/opp/vendors/cust/op_impl/aicpu/config"
        BACKUP_DIR="${CANN_ROOT}/opp/vendors/cust/${BACKUP_BASENAME}"
        if [[ "$ACTION" == "install" ]]; then
            if ! CANN_VERSION="$(get_cann_version "$CANN_ROOT")"; then
                echo "Error: cannot detect CANN version; CANN >= ${MIN_CANN_VERSION} is required." >&2
                exit 1
            fi
            if ! is_cann_version_supported "$CANN_VERSION"; then
                echo "Error: CANN ${CANN_VERSION} is not supported; CANN >= ${MIN_CANN_VERSION} is required." >&2
                exit 1
            fi
            echo "Detected supported CANN version: ${CANN_VERSION}"
            do_install "$SOURCE_DIR" "$KERNEL_DIR" "$CONFIG_DIR" "$BACKUP_DIR"
        else
            do_uninstall "$KERNEL_DIR" "$CONFIG_DIR" "$BACKUP_DIR"
        fi
        ;;
    info)
        if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
            echo "Error: ASCEND_HOME_PATH is not set. Set it to the CANN root directory." >&2
            exit 1
        fi
        show_info "${ASCEND_HOME_PATH%/}"
        ;;
esac
