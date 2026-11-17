#!/usr/bin/env python3
import argparse
import json
import re
import subprocess
import sys
from pathlib import Path


def command(*args):
    try:
        return subprocess.run(args, check=True, capture_output=True, text=True).stdout
    except (FileNotFoundError, subprocess.CalledProcessError) as error:
        detail = getattr(error, "stderr", "") or str(error)
        raise SystemExit(f"[ERROR] {' '.join(args)} 失败: {detail.strip()}") from error


def main():
    parser = argparse.ArgumentParser(description="核对 1825 数据面网卡并启用优先级 4 PFC")
    parser.add_argument("--apply", action="store_true", help="保存原配置并执行修改")
    parser.add_argument("--backup", help="修改前备份文件路径，不能已存在")
    parser.add_argument("nic", nargs="+", help="规划中的全部数据面网卡名称")
    args = parser.parse_args()
    selected = set(args.nic)
    if len(selected) != len(args.nic):
        raise SystemExit("[ERROR] 数据面网卡列表存在重复项")
    if args.apply and not args.backup:
        raise SystemExit("[ERROR] --apply 需要 --backup 保存原配置")

    cards = {}
    card = None
    for line in command("hinicadm5", "info").splitlines():
        match = re.search(r"\|----(hinic\d+)\(", line)
        if match:
            card = match.group(1)
            cards[card] = []
        match = re.search(r"\(NIC:([^()]+)\)", line)
        if match and card:
            cards[card].append(match.group(1))
    planned = []
    matched = set()
    for card, interfaces in cards.items():
        chosen = selected.intersection(interfaces)
        if not chosen:
            continue
        if len(interfaces) != 2 or chosen != set(interfaces):
            raise SystemExit(f"[ERROR] {card} 的两个网口未全部包含在数据面计划中: {interfaces}")
        info = command("hinicadm5", "info", "-i", card)
        if not re.search(r"(?m)^port num\s*:\s*2\s*$", info):
            raise SystemExit(f"[ERROR] {card} 不是已验证的双端口网卡")
        matched.update(chosen)
        for port in (0, 1):
            output = command("hinicadm5", "qos", "-i", card, "-t", "pfc", "-p", str(port))
            state = re.search(r"pfc state:\s*(enable|disable)", output)
            bitmap = re.search(r"(?m)^pfcup:\s*((?:[01]\s+){7}[01])\s*$", output)
            if not state or not bitmap or state.group(1) != "enable":
                raise SystemExit(f"[ERROR] {card}:{port} 的 PFC 状态不是已启用或无法解析，请人工核对")
            before = [int(bit) for bit in bitmap.group(1).split()]
            after = before.copy()
            after[4] = 1
            if sum(after) > 4:
                raise SystemExit(f"[ERROR] {card}:{port} 添加优先级 4 后超过网卡支持的 4 个优先级")
            planned.append({"card": card, "port": port, "before": before, "after": after})
    if matched != selected:
        raise SystemExit(f"[ERROR] 未能确定所属网卡的接口: {', '.join(sorted(selected - matched))}")
    for entry in planned:
        print(f"{entry['card']}:{entry['port']} {entry['before']} -> {entry['after']}")
    if not args.apply:
        print("[OK] 仅生成计划，未修改网卡。")
        return
    backup = Path(args.backup)
    try:
        with backup.open("x", encoding="utf-8") as file:
            file.write(json.dumps(planned, ensure_ascii=False, indent=2) + "\n")
        backup.chmod(0o600)
    except OSError as error:
        raise SystemExit(f"[ERROR] 无法保存 PFC 原配置 {backup}: {error}") from error
    for entry in planned:
        bitmap = ",".join(str(bit) for bit in entry["after"])
        command("hinicadm5", "qos", "-i", entry["card"], "-t", "pfc", "-p", str(entry["port"]), "-e", "1", "-f", bitmap)
        output = command("hinicadm5", "qos", "-i", entry["card"], "-t", "pfc", "-p", str(entry["port"]))
        found = re.search(r"(?m)^pfcup:\s*((?:[01]\s+){7}[01])\s*$", output)
        if not found or [int(bit) for bit in found.group(1).split()] != entry["after"]:
            raise SystemExit(f"[ERROR] {entry['card']}:{entry['port']} PFC 验证失败，原配置见 {backup}")
    print(f"[OK] 已启用 PFC 4，原配置保存在 {backup}。")


if __name__ == "__main__":
    main()
