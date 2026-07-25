#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PACKAGE_NAME="cann-hybm-probe.tar.gz"
JSON_NAME="libcann_hybm_probe_kernel.json"
PROBE_VENDOR_ROOT="opp/vendors/cust/mf_hybm_probe"
PACKAGE_PATH="${PROBE_VENDOR_ROOT}/op_impl/aicpu/kernel"
CONFIG_PATH="${PROBE_VENDOR_ROOT}/op_impl/aicpu/config"

if [[ -z "${ASCEND_HOME_PATH:-}" ]]; then
    echo "ASCEND_HOME_PATH is not set" >&2
    exit 1
fi

INI_FILE="${ASCEND_HOME_PATH}/conf/ascend_package_load.ini"
PACKAGE_DIR="${ASCEND_HOME_PATH}/${PACKAGE_PATH}"
CONFIG_DIR="${ASCEND_HOME_PATH}/${CONFIG_PATH}"

remove_ini_block()
{
    if [[ -f "${INI_FILE}" ]]; then
        sed -i '/^name:cann-hybm-probe.tar.gz$/,+4d' "${INI_FILE}"
    fi
}

if [[ "${1:-install}" == "uninstall" ]]; then
    rm -f "${PACKAGE_DIR}/${PACKAGE_NAME}" "${CONFIG_DIR}/${JSON_NAME}"
    remove_ini_block
    echo "Uninstalled HYBM route probe artifacts"
    exit 0
fi

if [[ ! -f "${SCRIPT_DIR}/${PACKAGE_NAME}" || ! -f "${SCRIPT_DIR}/${JSON_NAME}" ]]; then
    echo "Probe package or JSON is missing in ${SCRIPT_DIR}" >&2
    exit 1
fi

mkdir -p "${PACKAGE_DIR}" "${CONFIG_DIR}" "$(dirname "${INI_FILE}")"
cp -f "${SCRIPT_DIR}/${PACKAGE_NAME}" "${PACKAGE_DIR}/"
cp -f "${SCRIPT_DIR}/${JSON_NAME}" "${CONFIG_DIR}/"
remove_ini_block
if [[ -s "${INI_FILE}" ]] && [[ "$(tail -c 1 "${INI_FILE}" | od -An -tx1 | tr -d ' \n')" != "0a" ]]; then
    printf '\n' >> "${INI_FILE}"
fi
cat >> "${INI_FILE}" <<EOF
name:${PACKAGE_NAME}
install_path:2
optional:true
package_path:${PACKAGE_PATH}
load_as_per_soc:false
EOF

echo "Installed probe package: ${PACKAGE_DIR}/${PACKAGE_NAME}"
echo "Installed probe JSON: ${CONFIG_DIR}/${JSON_NAME}"
echo "export MF_HYBM_AICPU_KERNEL_JSON=${CONFIG_DIR}/${JSON_NAME}"
