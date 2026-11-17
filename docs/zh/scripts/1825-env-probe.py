#!/usr/bin/env python3
import argparse
import ipaddress
import json
import re
import shlex
import subprocess
import sys
from pathlib import Path


def command(*args):
    try:
        return subprocess.run(args, check=True, capture_output=True, text=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError) as error:
        print(f"[WARN] 无法采集 {' '.join(args)}: {error}", file=sys.stderr)
        return ""


def array(name, values, source):
    return f"{name}=({' '.join(shlex.quote(str(value)) for value in values)})  # {source}\n"


def blocks(text):
    result = {}
    for match in re.finditer(r"(?m)^interface\s+(\S+)\s*\n(.*?)(?=^#\s*$|^return\s*$)", text, re.S):
        result[match.group(1)] = match.group(2)
    return result


def qos_trust(text):
    modes = {"dscp": "trust dscp", "outer 8021p": "trust 8021p outer", "inner 8021p": "trust 8021p inner"}
    result = {}
    pattern = r"(?m)^[ \t]*interface[ \t]+(\S+)[ \t]*\n[ \t]*-+[ \t]*\n[ \t]*trust flag[ \t]*:[ \t]*([^\n]+)"
    for match in re.finditer(pattern, text):
        result[match.group(1)] = modes.get(match.group(2).strip().lower(), "")
    return result


def free_tables():
    occupied = set(range(0, 100)) | {253, 254, 255}
    aliases = {}
    files = [Path("/etc/iproute2/rt_tables")]
    files.extend(Path("/etc/iproute2/rt_tables.d").glob("*.conf"))
    for path in files:
        if path.is_file():
            for line in path.read_text(errors="replace").splitlines():
                match = re.match(r"^\s*(\d+)\s+(\S+)", line)
                if match:
                    occupied.add(int(match.group(1)))
                    aliases[match.group(2)] = match.group(1)
    for family in ("-4", "-6"):
        for kind in ("rule", "route"):
            args = ["ip", "-j", family, kind, "show"]
            if kind == "route":
                args += ["table", "all"]
            output = command(*args)
            if not output:
                return []
            try:
                for item in json.loads(output):
                    table = str(item.get("table", ""))
                    table = aliases.get(table, table)
                    if table.isdigit():
                        occupied.add(int(table))
            except (TypeError, ValueError):
                print("[WARN] 无法解析路由表或策略规则", file=sys.stderr)
                return []
    return [number for number in range(100, 253) if number not in occupied]


def topology(hcas):
    topo = command("npu-smi", "info", "-t", "topo")
    mapping = command("npu-smi", "info", "-m")
    device_ids = {}
    for line in mapping.splitlines():
        fields = line.split()
        if len(fields) >= 4 and all(field.isdigit() for field in fields[:4]):
            npu, physical = int(fields[0]), int(fields[3])
            if npu in device_ids and device_ids[npu] != physical:
                print(f"[WARN] NPU {npu} 对应多个物理 device，需人工确认", file=sys.stderr)
                return {}
            device_ids[npu] = physical
    names = {int(k): name for k, name in re.findall(r"(?m)^NIC(\d+):\s*(\S+)", topo)}
    header = next((line.split() for line in topo.splitlines() if re.match(r"^\s*NPU0\s+NPU1\s", line)), [])
    npu_columns = [int(field[3:]) for field in header if re.fullmatch(r"NPU\d+", field)]
    devices = {}
    for line in topo.splitlines():
        fields = line.split()
        if not fields or not re.fullmatch(r"NIC\d+", fields[0]):
            continue
        hca = names.get(int(fields[0][3:]))
        nic = hcas.get(hca)
        if not nic:
            continue
        physical = []
        for npu, relation in zip(npu_columns, fields[1:]):
            if relation == "UB" or relation.startswith("UB&"):
                if npu not in device_ids:
                    physical = []
                    break
                physical.append(device_ids[npu])
        if physical:
            devices[nic] = sorted(physical)
    return devices


def main():
    parser = argparse.ArgumentParser(description="只读采集 1825 主机信息并生成待确认的环境文件")
    parser.add_argument("--out", help="输出的 Bash 环境文件；不会覆盖现有文件")
    parser.add_argument("--switch-snapshot", help="包含交换机端口和 Vlanif 的 display 回显")
    parser.add_argument("--host-snapshot", help="清理前 ip -j address show 的 JSON 回显")
    args = parser.parse_args()
    output = Path(args.out).expanduser().resolve() if args.out else None
    if output and output.exists():
        raise SystemExit(f"[ERROR] 输出文件已存在，未覆盖: {output}")

    hcas = {}
    for path in sorted(Path("/sys/class/infiniband").glob("*")):
        interfaces = sorted((path / "device/net").glob("*"))
        if len(interfaces) == 1:
            hcas[path.name] = interfaces[0].name
        else:
            print(f"[WARN] {path.name} 的网卡数量不是 1，需人工确认", file=sys.stderr)
    if not hcas:
        raise SystemExit("[ERROR] 未从 sysfs 发现 HCA 与网卡映射")

    devices = topology(hcas)
    try:
        lldp = json.loads(command("lldpcli", "-f", "json", "show", "neighbors"))
        interfaces = lldp.get("lldp", {}).get("interface", [])
        if isinstance(interfaces, dict):
            interfaces = [interfaces]
        neighbors = {}
        for entry in interfaces:
            for nic, detail in entry.items():
                if nic not in hcas.values():
                    continue
                port = detail.get("port", {}).get("id", {})
                chassis = detail.get("chassis", {})
                neighbors[nic] = (
                    port.get("value", "") if port.get("type") == "ifname" else "",
                    next(iter(chassis), ""),
                )
    except (ValueError, TypeError, AttributeError):
        print("[WARN] 无法解析 LLDP，交换机子口需人工填写", file=sys.stderr)
        neighbors = {}

    try:
        address_data = (
            Path(args.host_snapshot).read_text() if args.host_snapshot else command("ip", "-j", "address", "show")
        )
        addresses = {entry["ifname"]: entry.get("addr_info", []) for entry in json.loads(address_data)}
        defaults = json.loads(command("ip", "-j", "-4", "route", "show", "default"))
    except (ValueError, KeyError, TypeError):
        raise SystemExit("[ERROR] 无法采集主机地址或管理路由")
    management = defaults[0].get("dev", "") if defaults else ""
    snapshot = Path(args.switch_snapshot).read_text(errors="replace") if args.switch_snapshot else ""
    switch = blocks(snapshot)
    trust_modes = qos_trust(snapshot)
    nics = sorted(set(hcas.values()), key=lambda nic: (neighbors.get(nic, ("", ""))[0] or "~", nic))
    sport, dev, vlan, gw4, gw6, host4, host6, net4, net6, p4, p6, transceiver = ([] for _ in range(12))
    host_mtu, vlan_mtu = [], []
    qos_profiles, chassis_names = [], set()
    for nic in nics:
        port, chassis = neighbors.get(nic, ("", ""))
        if chassis:
            chassis_names.add(chassis)
        sport.append(port)
        dev.append(",".join(map(str, devices.get(nic, []))))
        current = switch.get(port, "")
        match = re.search(r"(?m)^\s*port default vlan\s+(\d+)", current)
        value = match.group(1) if match else ""
        vlan.append(value)
        vconfig = switch.get("Vlanif" + value, "") if value else ""
        ipv4 = re.search(r"(?m)^\s*ip address\s+(\S+)\s+(\S+)", vconfig)
        ipv6 = re.search(r"(?m)^\s*ipv6 address\s+(\S+)", vconfig)
        mtu = re.search(r"(?m)^\s*mtu\s+(\d+)\s*$", vconfig)
        gw4.append(ipv4.group(1) if ipv4 else "")
        gw6.append(ipv6.group(1).split("/")[0] if ipv6 else "")
        vlan_mtu.append(mtu.group(1) if mtu else "")
        mtu_path = Path('/sys/class/net') / nic / 'mtu'
        host_mtu.append(mtu_path.read_text().strip() if mtu_path.is_file() else "")
        selected = {}
        for entry in addresses.get(nic, []):
            if entry.get("scope") == "global" and entry.get("family") in ("inet", "inet6"):
                selected.setdefault(entry["family"], []).append(entry)
        for family, hosts, nets, prefixes in (("inet", host4, net4, p4), ("inet6", host6, net6, p6)):
            items = selected.get(family, [])
            if len(items) == 1:
                entry = items[0]
                interface = ipaddress.ip_interface(f"{entry['local']}/{entry['prefixlen']}")
                hosts.append(str(interface.ip))
                nets.append(str(interface.network.network_address))
                prefixes.append(entry["prefixlen"])
            else:
                hosts.append("")
                nets.append("")
                prefixes.append("")
        match = re.search(r"(?m)^\s*device transceiver\s+(\S+)", current)
        transceiver.append(match.group(1) if match else "")
        profile = [
            line.strip()
            for line in current.splitlines()
            if re.match(r"^\s*(?:trust (?:upstream\b|dscp(?:\s|$)|8021p(?:\s|$))|qos |dcb pfc )", line)
        ]
        if port in trust_modes:
            profile = [line for line in profile if not re.fullmatch(r"trust (?:dscp|8021p(?: (?:inner|outer))?)", line)]
            if trust_modes[port]:
                profile.append(trust_modes[port])
            else:
                profile = []
                print(f"[WARN] {port} 的信任方式无法识别，需人工确认", file=sys.stderr)
        qos_profiles.append(profile)

    if len(chassis_names) > 1:
        print("[WARN] LLDP 发现多个交换机，必须人工核对端口归属", file=sys.stderr)
    qoscmds = (
        qos_profiles[0]
        if qos_profiles and all(qos_profiles) and all(profile == qos_profiles[0] for profile in qos_profiles)
        else []
    )
    tables = free_tables()[: len(nics)]
    tables.extend([""] * (len(nics) - len(tables)))
    fields = [
        ("SPORT", sport, "LLDP；缺失时手工填写"),
        ("NIC", nics, "HCA sysfs 与 LLDP"),
        ("DEV", dev, "NPU topo、NIC 到 HCA 对照及物理 ID；需确认"),
        ("SVLAN", vlan, "交换机快照；无快照时由网络管理员分配"),
        ("SV4GW", gw4, "交换机快照或人工地址规划"),
        ("SV6GW", gw6, "交换机快照或人工地址规划"),
        ("V4NET", net4, "主机地址或快照；空白时人工规划"),
        ("V4HOST", host4, "主机地址或快照；空白时人工规划"),
        ("V4LEN", p4, "主机地址或快照的前缀长度；空白时人工规划"),
        ("V6PFX", net6, "主机地址或快照；空白时人工规划"),
        ("V6HOST", host6, "主机地址或快照；空白时人工规划"),
        ("V6LEN", p6, "主机地址或快照的前缀长度；空白时人工规划"),
        ("TABLE", tables, "仅排除本机当前路由、规则和注册表占用；仍需确认预留"),
        ("HOST_MTU", host_mtu, "主机当前 MTU；需确认目标值"),
        ("VLAN_MTU", vlan_mtu, "交换机 Vlanif 快照；无快照时人工规划"),
        ("TRANSCEIVER", transceiver, "交换机快照；空白则不配置"),
        ("SW_QOS_CMDS", qoscmds, "快照中八口一致的 QoS/PFC；否则人工填写"),
    ]
    result = (
        "#!/bin/bash\n# 此文件含现场采集值和人工规划值。核对后将 PLAN_CONFIRMED 改为 1。\n"
        "PLAN_CONFIRMED=0\nPROTO=v6  # v4、v6 或 both；可在命令前用 PROTO 覆盖\n"
    )
    result += "MGT_IF=" + shlex.quote(management) + "  # IPv4 默认路由的接口，需确认\n"
    for name, values, source in fields:
        result += array(name, values, source)
    if output:
        output.parent.mkdir(parents=True, exist_ok=True)
        try:
            with output.open("x", encoding="utf-8") as target:
                target.write(result)
        except FileExistsError:
            raise SystemExit(f"[ERROR] 输出文件已存在，未覆盖: {output}")
        print(f"已生成 {output}；未修改网络配置。")
    else:
        print("只读采集完成；如需生成环境文件，请指定 --out。")
    print("网卡、交换机端口、device 来源：sysfs、LLDP、NPU topo；空白项需人工确认。")
    print("VLAN、网关、QoS 来源：交换机快照；无快照时需人工规划。")
    for values in zip(nics, sport, dev, vlan, gw4, gw6, tables):
        print("  " + " | ".join(str(value) or "待填写" for value in values))
    if not qoscmds:
        print("[WARN] QoS/PFC 未采集或各口不一致，必须人工填写 SW_QOS_CMDS。")
    print("核对所有映射、VLAN/IP 规划、表号和管理口后，将 PLAN_CONFIRMED 改为 1。")


if __name__ == "__main__":
    main()
