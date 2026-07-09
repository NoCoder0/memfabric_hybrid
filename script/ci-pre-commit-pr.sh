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

set -euo pipefail

show_help()
{
    echo "Usage: $0"
    echo ""
    echo "Run repository pre-commit hooks on all files."
    echo ""
    echo "Important: this script is intended for repository-wide formatting/checking."
    echo "Formatting must not change code logic. If hooks modify files, review the"
    echo "diff carefully and keep only formatting-equivalent changes."
}

install_pre_commit_if_needed()
{
    if command -v pre-commit >/dev/null 2>&1; then
        return 0
    fi

    echo "[INFO] pre-commit command not found, installing with python3 -m pip --user"
    if ! command -v python3 >/dev/null 2>&1; then
        echo "[ERROR] python3 command not found, please install pre-commit manually"
        exit 127
    fi

    python3 -m pip install --user pre-commit
    export PATH="${HOME}/.local/bin:${PATH}"

    if ! command -v pre-commit >/dev/null 2>&1; then
        echo "[ERROR] pre-commit still not found after installation"
        exit 127
    fi
}

run_pre_commit_all_files()
{
    echo -e "\n[INFO] 开始 pre-commit 全量检查/格式化"
    echo "[COMMAND] pre-commit run --all-files --show-diff-on-failure"

    set +e
    pre-commit run --all-files --show-diff-on-failure
    local code=$?
    set -e

    return "${code}"
}

print_result()
{
    local code=$1

    echo -e "\n================================================================"
    if [ "${code}" -eq 0 ]; then
        echo "[INFO] pre-commit 全量检查全部通过"
    else
        echo "[ERROR] pre-commit 全量检查失败或自动修改了文件"
        echo "[INFO] 如果 hook 自动修改了文件，请务必执行以下检查后再提交："
        echo ""
        echo "1. 查看改动"
        echo "git diff --stat"
        echo "git diff"
        echo ""
        echo "2. 确认所有改动都只是格式化等价变更，不改变代码逻辑"
        echo ""
        echo "3. 修复后重新执行"
        echo "$0"
    fi
    echo "================================================================"
}

main()
{
    if [[ "${1:-}" == "--help" || "${1:-}" == "-h" ]]; then
        show_help
        exit 0
    fi

    if [[ $# -ne 0 ]]; then
        echo "[ERROR] unsupported arguments: $*"
        show_help
        exit 2
    fi

    echo "================================================"
    echo "          Pre-Commit 全量检查/格式化"
    echo "================================================"
    echo "[INFO] 模式: 全量，不按 MR/PR 变更文件做增量过滤"

    git config core.quotePath false
    install_pre_commit_if_needed

    local code=0
    run_pre_commit_all_files || code=$?
    print_result "${code}"

    exit "${code}"
}

main "$@"
