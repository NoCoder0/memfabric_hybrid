#!/usr/bin/env python3
import os
import re
import secrets
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def main():
    if len(sys.argv) != 8 or sys.argv[1] not in ('v4', 'v6', 'both'):
        raise SystemExit('[ERROR] 需要协议类型、两个网卡、两个 IPv4 和两个 IPv6 地址')
    if not shutil.which('tcpdump') or not shutil.which('ping'):
        raise SystemExit('[ERROR] 宿主机需要 tcpdump 和支持 -e 的 ping')
    protocol, first, second, ip4a, ip4b, ip6a, ip6b = sys.argv[1:]
    out = Path(tempfile.mkdtemp(prefix='mf-1825-path-'))
    print(f'原始抓包文本和 ping 日志: {out}', flush=True)
    cases = [
        ('-6', ip6a, ip6b, first, second),
        ('-6', ip6b, ip6a, second, first),
        ('-4', ip4a, ip4b, first, second),
        ('-4', ip4b, ip4a, second, first),
    ]
    cases = [case for case in cases if protocol == 'both' or case[0] == ('-4' if protocol == 'v4' else '-6')]
    for index, (family, source, destination, source_dev, destination_dev) in enumerate(cases):
        ident = secrets.randbelow(64510) + 1024
        packet = (
            f'icmp6 and ip6[40] = 128 and ip6[44:2] = {ident}'
            if family == '-6'
            else f'icmp and icmp[0] = 8 and icmp[4:2] = {ident}'
        )
        packet += f' and src host {source} and dst host {destination}'
        captures, streams = [], []
        try:
            for side, dev, direction in [('source', source_dev, 'out'), ('destination', destination_dev, 'in')]:
                data = out / f'{index}-{side}.txt'
                error = out / f'{index}-{side}.stderr'
                data_file = data.open('w')
                error_file = error.open('w')
                streams.extend([data_file, error_file])
                process = subprocess.Popen(
                    ['tcpdump', '-nn', '-e', '-l', '-i', dev, '-Q', direction, '-c', '3', packet],
                    stdout=data_file,
                    stderr=error_file,
                )
                captures.append((side, process, data, error))
            for _ in range(50):
                if all('listening on' in error.read_text(errors='replace') for _, _, _, error in captures):
                    break
                if any(process.poll() is not None for _, process, _, _ in captures):
                    raise RuntimeError(f'{source} -> {destination}: tcpdump 启动失败，查看 {out}')
                time.sleep(0.1)
            else:
                raise RuntimeError(f'{source} -> {destination}: tcpdump 未就绪，查看 {out}')
            ping = subprocess.run(
                ['ping', family, '-e', str(ident), '-n', '-c', '3', '-i', '0.3', '-W', '2', '-I', source, destination],
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                text=True,
                timeout=15,
                env={**os.environ, 'LC_ALL': 'C'},
            )
            (out / f'{index}-ping.txt').write_text(ping.stdout)
            if ping.returncode or '3 packets transmitted, 3 received' not in ping.stdout:
                raise RuntimeError(f'{source} -> {destination}: ping 未全部收到回复，查看 {out}')
            for side, process, _, error in captures:
                try:
                    status = process.wait(timeout=6)
                except subprocess.TimeoutExpired as exc:
                    raise RuntimeError(f'{source} -> {destination}: {side} 未抓到 3 个报文，查看 {out}') from exc
                if status:
                    raise RuntimeError(f'{source} -> {destination}: {side} 抓包返回 {status}，查看 {error}')
            for stream in streams:
                stream.flush()
            for side, _, data, _ in captures:
                lines = data.read_text(errors='replace').splitlines()
                matches = [re.search(r'id ([0-9]+), seq ([0-9]+)', line) for line in lines]
                if len(matches) != 3 or any(match is None for match in matches):
                    raise RuntimeError(f'{source} -> {destination}: {side} 抓包数量或格式不符，查看 {data}')
                if any(int(match.group(1)) != ident for match in matches) or {
                    int(match.group(2)) for match in matches
                } != {1, 2, 3}:
                    raise RuntimeError(f'{source} -> {destination}: {side} 的 ICMP 编号不符，查看 {data}')
            print(f'[OK] {source} -> {destination}: {source_dev} 发出、{destination_dev} 收到各 3 个匹配请求')
        finally:
            for _, process, _, _ in captures:
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=2)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()
            for stream in streams:
                stream.close()
    print(f'[OK] {protocol} 双向 ICMP 均经过两端网卡；RoCE 硬件卸载仍需结合交换机计数验证')


if __name__ == '__main__':
    main()
