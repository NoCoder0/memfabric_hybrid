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
CUR_DIR=$(dirname $(readlink -f $0))

function print()
{
    echo "[${1}] ${2}"
}

function delete_install_files()
{
    if [ -z "$1" ]; then
        return 0
    fi

    install_dir=$1
    print "INFO" "memfabric_hybrid $(basename $1) delete install files!"
    if [ -f ${install_dir} ]; then
        chmod 700 ${install_dir}
        rm -f ${install_dir}
    elif [ -d ${install_dir} ]; then
        chmod -R 700 ${install_dir}
        rm -rf ${install_dir}
    fi
}

function delete_latest()
{
    print "INFO" "memfabric_hybrid delete latest!"
    cd $1/..
    if [ -f "set_env.sh" ]; then
        chmod 500 set_env.sh
        rm -rf set_env.sh
    fi
    if [ -d "latest" ]; then
        chmod -R 700 latest
        rm -rf latest
    fi
}

function is_ops_installed()
{
    local cann_root="$1"
    local tar_file="${cann_root}/opp/vendors/cust/op_impl/aicpu/kernel/cann-hybm-compat.tar.gz"
    local json_file="${cann_root}/opp/vendors/cust/op_impl/aicpu/config/libcann_hybm_kernel.json"
    local version_file="${cann_root}/opp/vendors/cust/op_impl/aicpu/config/cann_hybm_kernel_version"
    local ini_file="${cann_root}/conf/ascend_package_load.ini"

    if [ -f "${tar_file}" ] || [ -f "${json_file}" ] || [ -f "${version_file}" ]; then
        return 0
    fi
    if [ -f "${ini_file}" ] && grep -q '^name:cann-hybm-compat.tar.gz$' "${ini_file}" 2>/dev/null; then
        return 0
    fi
    return 1
}

function uninstall_ops_if_installed()
{
    local cann_root="${ASCEND_HOME_PATH:-}"
    local saved_cann_root="${CUR_DIR}/ops/cann_root"
    if [ -z "${cann_root}" ] && [ -f "${saved_cann_root}" ]; then
        cann_root="$(head -n 1 "${saved_cann_root}")"
        print "INFO" "Use CANN path saved during OPS installation: ${cann_root}"
    fi
    if [ -z "${cann_root}" ]; then
        print "WARNING" "CANN path is unavailable, skip HYBM OPS uninstall detection."
        return 0
    fi

    cann_root="${cann_root%/}"
    if [ ! -d "${cann_root}" ]; then
        print "WARNING" "CANN directory does not exist, skip HYBM OPS uninstall detection: ${cann_root}"
        return 0
    fi
    if ! is_ops_installed "${cann_root}"; then
        print "INFO" "HYBM OPS is not installed, skip uninstall."
        return 0
    fi

    local ops_installer="${CUR_DIR}/ops/install.sh"
    print "INFO" "Detected installed HYBM OPS, start uninstall."
    if [ -f "${ops_installer}" ]; then
        if ! ASCEND_HOME_PATH="${cann_root}" bash "${ops_installer}" --uninstall; then
            print "ERROR" "HYBM OPS uninstall failed."
            return 1
        fi
    elif command -v mfcli >/dev/null 2>&1; then
        if ! ASCEND_HOME_PATH="${cann_root}" mfcli aicpu uninstall; then
            print "ERROR" "HYBM OPS uninstall failed."
            return 1
        fi
    else
        print "ERROR" "HYBM OPS is installed, but no OPS uninstaller is available."
        return 1
    fi
    print "INFO" "HYBM OPS uninstall success."
}

function uninstall_process()
{
    if [ ! -d $1 ]; then
        return 0
    fi
    print "INFO" "memfabric_hybrid $(basename $1) uninstall start.."
    mf_dir=$(cd $1/..;pwd)
    delete_latest $1
    delete_install_files $1
    if [ -z "$(ls $mf_dir)" ]; then
        chmod -R 700 $mf_dir
        rm -rf $mf_dir
    fi

    cd ~ 2>/dev/null
    pip_path=$(which pip3 2>/dev/null)
    if [ -z "$pip_path" ]; then
        print "WARNING" "memfabric_hybrid  pip3 Not Found, skip uninstall wheel package."
    else
        pip3 uninstall -y memfabric_hybrid
    fi
    print "INFO" "memfabric_hybrid $(basename $1) uninstall success!"
}

install_dir=${CUR_DIR}
if ! uninstall_ops_if_installed; then
    exit 1
fi
uninstall_process ${install_dir}
