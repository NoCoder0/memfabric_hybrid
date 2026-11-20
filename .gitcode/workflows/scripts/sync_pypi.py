#!/usr/bin/env python3
# Copyright (c) Huawei Technologies Co., Ltd. 2026. All rights reserved.
# MemFabric_Hybrid is licensed under Mulan PSL v2.
# You can use this software according to the terms and conditions of the Mulan PSL v2.
# You may obtain a copy of Mulan PSL v2 at:
#          http://license.coscl.org.cn/MulanPSL2
# THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
# EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
# MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.

"""Synchronize locally built wheels to the Ascend PyPI repository."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from urllib.parse import quote, urlsplit


PUBLIC = "https://ascend.devcloud.huaweicloud.com/pypi/"
DEFAULT_ROOT = Path(__file__).resolve().parents[3] / "BeiMing_output"
COMPONENTS = ("memcache-hybrid", "memfabric-hybrid")
MANAGEMENT_URL_ENV = "ASCEND_DEVCLOUD_PYPI_MANAGEMENT_URL"
USER_ENV = "ASCEND_DEVCLOUD_PYPI_USERNAME"
PASSWORD_ENV = "ASCEND_DEVCLOUD_PYPI_PASSWORD"


class SyncError(Exception):
    pass


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def management_url():
    value = os.environ.get(MANAGEMENT_URL_ENV)
    if not value:
        raise SyncError(f"请配置 GitCode Actions 变量 {MANAGEMENT_URL_ENV}。")
    try:
        parsed = urlsplit(value)
    except ValueError:
        parsed = None
    if (
        parsed is None
        or parsed.scheme != "https"
        or not parsed.hostname
        or parsed.username is not None
        or re.search(r'[\s\x00-\x1f\x7f"\\?#]', value)
    ):
        raise SyncError(f"{MANAGEMENT_URL_ENV} 须为不带凭据、查询参数或片段的 HTTPS 仓库地址。")
    return value.rstrip("/") + "/"


def curl_config(username, password):
    value = username + ":" + password
    if any(c in value for c in "\r\n\x00") or ":" in username:
        raise SyncError("用户名或密码包含不支持的字符。")
    # curl config is passed through stdin, never in the command line or a file.
    return 'user = "' + value.replace("\\", "\\\\").replace('"', '\\"') + '"\n'


def make_url(base, relative):
    return base + quote(relative.as_posix(), safe="/")


def curl_base(curl):
    return [curl, "--disable", "--silent", "--show-error", "--connect-timeout", "30", "--max-time", "900"]


def child_environment():
    return {key: value for key, value in os.environ.items() if key not in (MANAGEMENT_URL_ENV, USER_ENV, PASSWORD_ENV)}


def management_request(curl, auth, url, upload=None):
    args = curl_base(curl) + [
        "--config",
        "-",
        "--proto",
        "=https",
        "--globoff",
        "--write-out",
        "\nCODEX_HTTP_STATUS:%{http_code}",
    ]
    if upload is None:
        args.append("--head")
    else:
        args += ["--upload-file", str(upload)]
    config = f'{auth}url = "{url}"\n'
    result = subprocess.run(
        args, input=config, text=True, capture_output=True, encoding="utf-8", errors="replace", env=child_environment()
    )
    if result.returncode:
        raise SyncError(f"管理仓请求失败（curl {result.returncode}）。")
    body, marker, status = result.stdout.rpartition("\nCODEX_HTTP_STATUS:")
    if not marker or not status.isdigit():
        raise SyncError("管理仓返回了无法解析的状态。")
    return int(status), body


def remote_checksum(headers):
    # Only inspect the last HTTP response, after a possible proxy CONNECT.
    parts = re.split(r"(?im)^HTTP/\S+\s+\d+[^\r\n]*\r?\n", headers)
    for line in parts[-1].splitlines():
        name, sep, value = line.partition(":")
        if sep and name.lower() == "x-checksum-sha256":
            return value.strip().lower()
    return None


def verify_public(curl, url, expected):
    args = curl_base(curl) + ["--fail", "--location", "--proto", "=https", "--proto-redir", "=https", url]
    digest = hashlib.sha256()
    with tempfile.TemporaryFile() as error_file:
        with subprocess.Popen(args, stdout=subprocess.PIPE, stderr=error_file, env=child_environment()) as proc:
            for block in iter(lambda: proc.stdout.read(1024 * 1024), b""):
                digest.update(block)
            code = proc.wait()
        error_file.seek(0)
        error = error_file.read().decode("utf-8", "replace").strip()
    if code:
        raise SyncError(f"公开地址下载失败（curl {code}）：{error}")
    if digest.hexdigest() != expected:
        raise SyncError("公开地址下载内容的 SHA-256 与本地文件不一致。")


def main():
    parser = argparse.ArgumentParser(description="检查并同步当前任务构建的 wheel 到 Ascend PyPI。")
    parser.add_argument("--root", type=Path, default=DEFAULT_ROOT, help="本次构建归档到 OBS 的 wheel 所在目录")
    parser.add_argument("--component", choices=COMPONENTS, required=True, help="当前仓库对应的 PyPI 包名")
    parser.add_argument("--version", required=True, help="VERSION 文件中的基础版本号，例如 1.5.0")
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--build", type=str.lower, help="内测包构建号，例如 b013")
    mode.add_argument("--nightly", action="store_true", help="同步 daily 构建生成的开发版本包")
    parser.add_argument("--apply", action="store_true", help="上传缺失文件并从公开地址下载校验，默认只检查")
    args = parser.parse_args()
    if not re.fullmatch(r"[0-9]+(?:\.[0-9]+)*", args.version):
        raise SyncError(f"无效的基础版本号：{args.version!r}")
    if args.nightly:
        destination = Path("nightly") / args.component
        version_pattern = re.escape(args.version) + r"\.dev[0-9]+"
    else:
        if not re.fullmatch(r"b[0-9]+", args.build):
            raise SyncError(f"无效的内测包构建号：{args.build!r}")
        destination = Path(args.component) / args.version
        version_pattern = re.escape(args.version + "+" + args.build)

    distribution = args.component.replace("-", "_")
    wheel_prefix = re.compile(rf"{distribution}-{version_pattern}-")
    root = args.root.resolve()
    files = sorted(
        path for path in root.glob(f"{distribution}-*.whl") if path.is_file() and wheel_prefix.match(path.name)
    )
    if not files:
        raise SyncError(f"在 {root} 中未找到 {args.component} {args.version} 对应的 wheel，目标目录为 {destination}。")

    management = management_url()
    username = os.environ.get(USER_ENV)
    password = os.environ.get(PASSWORD_ENV)
    if not username or not password:
        raise SyncError(f"请配置 GitCode Action 密钥 {USER_ENV} 和 {PASSWORD_ENV}。")
    curl = shutil.which("curl")
    if not curl:
        raise SyncError("找不到 curl，请先安装或将其加入 PATH。")
    auth = curl_config(username, password)
    pending = []
    conflicts = []
    print(f"检查 {len(files)} 个文件（{'上传模式' if args.apply else '仅检查模式'}）。", flush=True)
    for path in files:
        rel = destination / path.name
        checksum = sha256(path)
        status, headers = management_request(curl, auth, make_url(management, rel))
        if status == 404:
            state = "待上传"
            pending.append((path, rel, checksum))
        elif status == 200:
            remote = remote_checksum(headers)
            if remote != checksum:
                conflicts.append(rel)
                state = "冲突或缺少远端 SHA-256"
            else:
                state = "已存在且校验一致"
        else:
            raise SyncError(f"检查 {rel.as_posix()} 时返回 HTTP {status}，停止操作。")
        print(f"{state}: {rel.as_posix()}", flush=True)
    if conflicts:
        raise SyncError(f"发现 {len(conflicts)} 个同名冲突或无法校验的文件，未上传任何文件。")
    if not args.apply:
        print(f"检查完成：{len(pending)} 个待上传。添加 --apply 执行上传和公开下载校验。")
        return
    for path, rel, checksum in pending:
        status, body = management_request(curl, auth, make_url(management, rel), upload=path)
        if status not in (200, 201):
            raise SyncError(f"上传 {rel.as_posix()} 返回 HTTP {status}；请重新检查目标状态后再重试。")
        try:
            remote = json.loads(body).get("checksums", {}).get("sha256", "").lower()
        except (ValueError, AttributeError):
            remote = ""
        if remote != checksum:
            raise SyncError(f"上传 {rel.as_posix()} 后管理仓 SHA-256 缺失或不匹配。")
        print(f"上传并校验成功: {rel.as_posix()}", flush=True)
    for path in files:
        rel = destination / path.name
        verify_public(curl, make_url(PUBLIC, rel), sha256(path))
        print(f"公开下载校验成功: {rel.as_posix()}", flush=True)
    print(f"完成：新上传 {len(pending)} 个，公开下载校验通过 {len(files)} 个。")


if __name__ == "__main__":
    try:
        main()
    except (SyncError, OSError) as exc:
        print(f"错误: {exc}", file=sys.stderr)
        sys.exit(1)
