#!/bin/bash

# build_and_pack_run.sh
#         │
#         ▼
#      script/build.sh
#         │
#         ├─ 构建 MemFabric 主库
#         │
#         └─ script/kernel/hybm/build.sh
#               │
#               ├─ build HYBM AICPU kernel
#               ├─ 生成原始算子 tar 包
#               └─ 调用 sign.sh
#                        │
#                        ├─ 检查 SignaTrust client
#                        ├─ 选择 CRL
#                        └─ add_header_sign.py
#                               ├─ 添加 ESBC 头
#                               ├─ 生成 INI 摘要
#                               ├─ CMS 签名 INI
#                               ├─ CRL 转 DER
#                               └─ 绑定 CMS/INI/CRL
#         │
#         ▼
# make_run.sh
#         │
#         ├─ 收集主库和 wheel
#         └─ 生成 run 包

set -euo pipefail

SIGN_TOOL="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
PROJECT_ROOT="$(cd "$SIGN_TOOL/../.." && pwd)"
SIGN_FILE="${SIGN_FILE:-$PROJECT_ROOT/output/hybm/aicpu_kernel/cann-hybm-compat.tar.gz}"
SIGN_ROOT="${SIGNATRUST_ROOT:-/opt/signaturst}"
export SIGNATRUST_ROOT="$SIGN_ROOT"
SIGN_CLIENT="$SIGN_ROOT/signatrust_client"
CLIENT_CONFIG="$SIGN_ROOT/client.toml"
BUNDLED_CRL_FILE="$SIGN_TOOL/signature/SWSCRL.crl"
if [ -n "${SIGN_CRL_FILE:-}" ] && [ -f "$SIGN_CRL_FILE" ]; then
    CRL_FILE="$SIGN_CRL_FILE"
elif [ -f "$SIGN_ROOT/MF-CRL.crl" ]; then
    CRL_FILE="$SIGN_ROOT/MF-CRL.crl"
elif [ -f "$SIGN_ROOT/SWSCRL.crl" ]; then
    CRL_FILE="$SIGN_ROOT/SWSCRL.crl"
else
    CRL_FILE="$BUNDLED_CRL_FILE"
fi
export SIGN_CRL_FILE="$CRL_FILE"
SIGN_CONFIG="$PROJECT_ROOT/script/sign/hybm_check_cfg.xml"
SIGN_ENTRY="$PROJECT_ROOT/script/sign/add_header_sign.py"
PACKAGE_VERSION="${1:-$(tr -d '[:space:]' < "$PROJECT_ROOT/VERSION")}"

validate_file() {
    local file_path="$1"
    local file_name="$2"

    if [ ! -f "$file_path" ]; then
        echo "error: ${file_name} does not exist: ${file_path}" >&2
        exit 1
    fi
}

sign_file() {
    echo "sign_file src=$SIGN_FILE version=$PACKAGE_VERSION"
    python3 "$SIGN_ENTRY" \
        "$(dirname "$SIGN_FILE")" \
        true \
        --bios_check_cfg="$SIGN_CONFIG" \
        --version="$PACKAGE_VERSION"
}

skip_signing() {
    echo "info: SignaTrust client is unavailable, skip signing: $1"
    exit 0
}

if [ ! -d "$SIGN_ROOT" ]; then
    skip_signing "directory does not exist: $SIGN_ROOT"
fi
if [ ! -f "$SIGN_CLIENT" ]; then
    skip_signing "client does not exist: $SIGN_CLIENT"
fi
if [ ! -x "$SIGN_CLIENT" ]; then
    skip_signing "client is not executable: $SIGN_CLIENT"
fi
if [ ! -f "$CLIENT_CONFIG" ]; then
    skip_signing "client config does not exist: $CLIENT_CONFIG"
fi

validate_file "$SIGN_FILE" "build artifact"
validate_file "$CRL_FILE" "CRL file"
validate_file "$SIGN_CONFIG" "sign config"
validate_file "$SIGN_ENTRY" "sign entry"

echo "SIGN_TOOL=$SIGN_TOOL"
echo "PROJECT_ROOT=$PROJECT_ROOT"
echo "SIGN_FILE=$SIGN_FILE"
echo "SIGN_CLIENT=$SIGN_CLIENT"
echo "CLIENT_CONFIG=$CLIENT_CONFIG"
echo "CRL_FILE=$CRL_FILE"
echo "PACKAGE_VERSION=$PACKAGE_VERSION"

sign_file
