#!/usr/bin/env python3
import argparse
import importlib.util
import ipaddress
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path


def fail(message):
    raise SystemExit(f"[ERROR] {message}")


def main():
    parser = argparse.ArgumentParser(description="只读采集本机与直连交换机，生成待确认的 1825 环境文件")
    parser.add_argument("--out-dir", required=True, help="保存快照和环境文件的目录")
    parser.add_argument("--switch", help="交换机管理 IPv4；未指定时从 LLDP 自动发现")
    parser.add_argument("--switch-user", default="huawei", help="交换机 SSH 用户名，默认 huawei")
    parser.add_argument("--ssh-password-env", help="保存交换机密码的环境变量名；需要 sshpass")
    parser.add_argument("--ssh-batch", action="store_true", help="禁止 SSH 密码或密钥口令交互")
    args = parser.parse_args()

    if os.geteuid() != 0:
        fail("请在宿主机使用 root 执行")
    if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_.-]*", args.switch_user):
        fail("交换机用户名格式无效")
    ssh_prefix = []
    ssh_options = []
    if args.ssh_batch or args.ssh_password_env:
        ssh_options += ["-o", "StrictHostKeyChecking=accept-new"]
    if args.ssh_batch:
        ssh_options += ["-o", "BatchMode=yes"]
    ssh_env = None
    if args.ssh_password_env:
        if not re.fullmatch(r"[A-Za-z_][A-Za-z0-9_]*", args.ssh_password_env):
            fail("交换机密码环境变量名无效")
        if not os.environ.get(args.ssh_password_env):
            fail(f"交换机密码环境变量为空: {args.ssh_password_env}")
        if not shutil.which("sshpass"):
            fail("使用密码时需要安装 sshpass；也可配置 SSH 免密登录")
        ssh_prefix = ["sshpass", "-e"]
        ssh_env = {**os.environ, "SSHPASS": os.environ[args.ssh_password_env]}
    probe_path = Path(__file__).resolve().with_name("1825-env-probe.py")
    if not probe_path.is_file():
        fail(f"缺少采集依赖: {probe_path}")
    spec = importlib.util.spec_from_file_location("env_probe_1825", probe_path)
    probe = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(probe)

    os.umask(0o077)
    out = Path(args.out_dir).expanduser().resolve()
    out.mkdir(parents=True, exist_ok=True, mode=0o700)
    host_path = out / "host-address.json"
    switch_path = out / "switch-snapshot.txt"
    env_path = out / "env.sh"
    for path in (host_path, switch_path, env_path):
        if path.exists():
            fail(f"文件已存在，未覆盖: {path}；请选择新的 --out-dir")

    hcas = {}
    for path in Path("/sys/class/infiniband").glob("*"):
        interfaces = list((path / "device/net").glob("*"))
        if len(interfaces) != 1:
            fail(f"HCA {path.name} 的网卡数量不是 1，请核对拓扑")
        hcas[path.name] = interfaces[0].name
    if not hcas:
        fail("未发现 HCA 和网卡；请在 1825 宿主机运行并检查驱动")

    try:
        output = subprocess.run(
            ["lldpcli", "-f", "json", "show", "neighbors"], check=True, capture_output=True, text=True
        ).stdout
        entries = json.loads(output)["lldp"]["interface"]
        if isinstance(entries, dict):
            entries = [entries]
        neighbors = {}
        for entry in entries:
            for nic, detail in entry.items():
                if nic not in hcas.values():
                    continue
                chassis = detail["chassis"]
                port = detail["port"]["id"]
                if len(chassis) != 1 or port.get("type") != "ifname":
                    fail(f"{nic} 的 LLDP 交换机或端口信息不完整")
                name, info = next(iter(chassis.items()))
                management = info.get("mgmt-ip", [])
                if isinstance(management, str):
                    management = [management]
                if nic in neighbors:
                    fail(f"{nic} 发现多个 LLDP 邻居，请先核对")
                neighbors[nic] = (name, port["value"], management)
    except (FileNotFoundError, subprocess.CalledProcessError, KeyError, TypeError, ValueError) as error:
        fail(f"无法读取 LLDP 邻居: {error}")
    if set(neighbors) != set(hcas.values()):
        fail(f"以下网卡没有 LLDP 邻居: {', '.join(sorted(set(hcas.values()) - set(neighbors)))}")
    chassis_names = {value[0] for value in neighbors.values()}
    if len(chassis_names) != 1:
        fail(f"网卡连接了多个交换机: {', '.join(sorted(chassis_names))}；请人工采集并核对")
    chassis = next(iter(chassis_names))
    ports = sorted({value[1] for value in neighbors.values()})
    if len(ports) != len(neighbors) or any(not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9/.:_-]*", p) for p in ports):
        fail("LLDP 端口格式无效或多个网卡映射到同一端口")

    management_sets = []
    for _, _, values in neighbors.values():
        ips = set()
        for value in values:
            try:
                ip = ipaddress.ip_address(value)
                if ip.version == 4:
                    ips.add(str(ip))
            except (TypeError, ValueError):
                continue
        management_sets.append(ips)
    common_addresses = set.intersection(*management_sets)
    if args.switch:
        try:
            switch = str(ipaddress.IPv4Address(args.switch))
        except ipaddress.AddressValueError:
            fail("--switch 必须是交换机管理 IPv4")
        if common_addresses and switch not in common_addresses:
            fail(f"--switch {switch} 与 LLDP 管理地址 {', '.join(sorted(common_addresses))} 不符")
    elif len(common_addresses) == 1:
        switch = next(iter(common_addresses))
    else:
        fail("无法从所有 LLDP 邻居确定唯一的交换机 IPv4，请使用 --switch 指定")

    print(f"LLDP 交换机: {chassis}，管理地址: {switch}，端口: {', '.join(ports)}", flush=True)
    print("将通过 SSH 执行只读 display 命令；可能需要输入交换机密码。", flush=True)
    try:
        host_data = subprocess.run(["ip", "-j", "address", "show"], check=True, capture_output=True, text=True).stdout
        json.loads(host_data)
        with host_path.open("x", encoding="utf-8") as target:
            target.write(host_data)
    except (FileNotFoundError, subprocess.CalledProcessError, ValueError, FileExistsError) as error:
        fail(f"主机地址快照采集失败: {error}")

    def read_switch(commands):
        request = "\n".join(["screen-length 0 temporary", *commands, "quit", ""])
        try:
            result = subprocess.run(
                [
                    *ssh_prefix,
                    "ssh",
                    "-tt",
                    "-o",
                    "ConnectTimeout=15",
                    *ssh_options,
                    "-o",
                    "ServerAliveInterval=10",
                    f"{args.switch_user}@{switch}",
                ],
                input=request,
                capture_output=True,
                text=True,
                timeout=120,
                env=ssh_env,
            )
        except (FileNotFoundError, subprocess.TimeoutExpired) as error:
            fail(f"交换机 SSH 失败: {error}；已保存主机快照 {host_path}")
        if not re.search(rf"(?m)^<{re.escape(chassis)}>\s*", result.stdout):
            fail(f"交换机 SSH 失败或系统名与 LLDP 的 {chassis} 不一致: {result.stderr.strip()}")
        return result.stdout

    first = read_switch(
        [
            "display vlan",
            *[f"display current-configuration interface {port}" for port in ports],
            *[f"display qos configuration interface {port}" for port in ports],
        ]
    )
    blocks = probe.blocks(first)
    missing = [port for port in ports if port not in blocks]
    if missing:
        fail(f"交换机未返回端口配置: {', '.join(missing)}；请检查账号权限及设备 CLI")
    trust = probe.qos_trust(first)
    missing_trust = [port for port in ports if not trust.get(port)]
    if missing_trust:
        fail(f"交换机未返回可识别的端口信任方式: {', '.join(missing_trust)}；请检查 QoS 回显")
    vlans = sorted(
        {
            int(match.group(1))
            for port in ports
            for match in [re.search(r"(?m)^\s*port default vlan\s+(\d+)", blocks[port])]
            if match
        }
    )
    vlans = [vlan for vlan in vlans if vlan != 1]
    second = ""
    if vlans:
        print("继续读取 Vlanif 配置；可能需要再次输入交换机密码。", flush=True)
        second = read_switch([f"display current-configuration interface Vlanif {vlan}" for vlan in vlans])
        found = probe.blocks(second)
        missing_vlans = [vlan for vlan in vlans if f"Vlanif{vlan}" not in found]
        if missing_vlans:
            print(f"[WARN] 未读到 Vlanif {missing_vlans}；对应网关和 MTU 需人工填写。", file=sys.stderr)
    else:
        print("[WARN] 端口没有非默认 PVID；VLAN 和网关需人工规划。", file=sys.stderr)
    try:
        with switch_path.open("x", encoding="utf-8") as target:
            target.write(first + "\n" + second)
    except FileExistsError:
        fail(f"交换机快照已存在，未覆盖: {switch_path}")
    result = subprocess.run(
        [
            sys.executable,
            str(probe_path),
            "--out",
            str(env_path),
            "--host-snapshot",
            str(host_path),
            "--switch-snapshot",
            str(switch_path),
        ]
    )
    if result.returncode:
        fail(f"环境文件生成失败，退出码 {result.returncode}；快照保存在 {out}")
    print(f"主机快照: {host_path}\n交换机快照: {switch_path}\n环境文件: {env_path}")


if __name__ == "__main__":
    main()
