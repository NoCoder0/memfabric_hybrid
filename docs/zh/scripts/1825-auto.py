#!/usr/bin/env python3
import argparse
import csv
import ipaddress
import json
import os
import re
import selectors
import shlex
import shutil
import subprocess
import sys
import time
from pathlib import Path

SCRIPTS = Path(__file__).resolve().parent


def fail(message):
    raise RuntimeError(message)


def run(args, env=None, capture=False):
    print('+ ' + shlex.join(str(arg) for arg in args), flush=True)
    result = subprocess.run(args, env=env, text=True, capture_output=capture)
    if result.returncode:
        fail(
            f'命令失败，退出码 {result.returncode}: {args[0]} {args[1] if len(args) > 1 else ""}'
            + (f'\n{result.stderr.strip()}' if capture else '')
        )
    return result.stdout if capture else ''


def load_config(path):
    config_path = Path(path).expanduser().resolve()
    config = json.loads(config_path.read_text(encoding='utf-8'))
    if not isinstance(config, dict):
        fail('配置文件必须是 JSON 对象')
    required = (
        'plan_tsv',
        'machine_id',
        'switch_host',
        'switch_user',
        'protocol',
        'run_family',
        'host_suffix',
        'workspace',
    )
    for key in required:
        if key not in config or config[key] in ('', None):
            fail(f'配置文件缺少 {key}')
    if (config['protocol'], config['run_family']) not in {
        ('v4', 'ipv4'),
        ('v6', 'ipv6'),
        ('both', 'ipv4'),
        ('both', 'ipv6'),
    }:
        fail('protocol 与 run_family 不匹配')
    for key in ('plan_tsv', 'machine_id', 'switch_host', 'switch_user', 'protocol', 'run_family', 'workspace'):
        if not isinstance(config[key], str):
            fail(f'{key} 必须是字符串')
    config['switch_host'] = str(ipaddress.IPv4Address(config['switch_host']))
    if not isinstance(config['machine_id'], str) or not config['machine_id'].strip():
        fail('machine_id 必须是非空字符串')
    if not re.fullmatch(r'[A-Za-z_][A-Za-z0-9_.-]*', config['switch_user']):
        fail('switch_user 格式无效')
    suffix = config['host_suffix']
    if isinstance(suffix, bool) or not isinstance(suffix, int) or not 2 <= suffix <= 254:
        fail('host_suffix 必须是 2 到 254 的整数')
    for key in ('host_mtu', 'vlan_mtu'):
        config.setdefault(key, 9000)
        if isinstance(config[key], bool) or not isinstance(config[key], int) or not 576 <= config[key] <= 65535:
            fail(f'{key} 必须是 576 到 65535 的整数')
    for key in ('plan_tsv', 'workspace'):
        value = Path(config[key]).expanduser()
        if not value.is_absolute():
            fail(f'{key} 必须是绝对路径')
        config[key] = str(value)
    config.setdefault('vlans', {})
    config.setdefault('qos_commands', None)
    if not isinstance(config['vlans'], dict):
        fail('vlans 必须是按端口指定 VLAN 的对象')
    if config['qos_commands'] is not None and (
        not isinstance(config['qos_commands'], list)
        or not all(isinstance(item, str) for item in config['qos_commands'])
    ):
        fail('qos_commands 必须是字符串数组')
    password = config.get('switch_password', '')
    if not isinstance(password, str):
        fail('switch_password 必须是字符串')
    if password and config_path.stat().st_mode & 0o077:
        fail('配置文件包含交换机密码时，请先执行 chmod 600')
    if password and not shutil.which('sshpass'):
        fail('填写 switch_password 时需要安装 sshpass；也可配置 SSH 免密并留空')
    return config


def read_array(env_path, name):
    script = 'source "$1"; declare -p "$2" >/dev/null; eval \'printf "%s\\0" "${' + name + '[@]}"\''
    result = subprocess.run(['bash', '-c', script, 'bash', str(env_path), name], capture_output=True, check=True)
    return result.stdout.decode().split('\0')[:-1]


def read_plan(config, ports):
    rows = []
    physical = ''
    port_count = {}
    with open(config['plan_tsv'], encoding='utf-8-sig', newline='') as source:
        reader = csv.reader(source, delimiter='\t')
        next(reader, None)
        for line, row in enumerate(reader, 2):
            if len(row) != 5:
                fail(f'规划表第 {line} 行应有 5 列，实际 {len(row)} 列')
            if row[0].strip():
                physical = row[0].strip()
            if re.fullmatch(r'\d+', physical):
                port_count[physical] = port_count.get(physical, 0) + 1
            if not row[4].strip().startswith(config['machine_id']):
                continue
            if not re.fullmatch(r'\d+', physical):
                fail(f'规划表第 {line} 行交换机口无效: {physical}')
            subport = f'{physical}:{port_count[physical]}'
            vlan = config['vlans'].get(subport, int(physical) * 10 + port_count[physical])
            if isinstance(vlan, bool) or not isinstance(vlan, int) or not 2 <= vlan <= 4094:
                fail(f'{subport} 的 VLAN 无效: {vlan}')
            entry = {'port': subport, 'vlan': vlan}
            for family, value in ((4, row[2].strip()), (6, row[3].strip())):
                selected = config['protocol'] == 'both' or config['protocol'] == f'v{family}'
                if not selected:
                    entry[f'gw{family}'] = entry[f'net{family}'] = entry[f'host{family}'] = ''
                    continue
                gateway = ipaddress.ip_address(value)
                prefix = 24 if family == 4 else 64
                network = ipaddress.ip_network(f'{gateway}/{prefix}', strict=False)
                if gateway.version != family or int(gateway) != int(network.network_address) + 1:
                    fail(f'规划表第 {line} 行 IPv{family} 网关必须为 /{prefix} 网段的 .1 或 ::1')
                offset = config['host_suffix'] if family == 4 else int(str(config['host_suffix']), 16)
                host = network.network_address + offset
                if family == 4 and host == network.broadcast_address:
                    fail(f'规划表第 {line} 行 IPv4 主机地址是广播地址')
                entry[f'gw{family}'] = str(gateway)
                entry[f'net{family}'] = str(network.network_address)
                entry[f'host{family}'] = str(host)
            rows.append(entry)
    expected = [port.rsplit('/', 1)[-1] for port in ports]
    if [row['port'] for row in rows] != expected:
        fail(f'规划表端口顺序与 LLDP 不一致: 表={[row["port"] for row in rows]} LLDP={expected}')
    if len({row['vlan'] for row in rows}) != len(rows):
        fail('规划表中 VLAN 重复')
    if set(config['vlans']) - {row['port'] for row in rows}:
        fail('vlans 包含规划表中不存在的端口')
    return rows


def write_plan(env_path, config, rows):
    values = {
        'SVLAN': [row['vlan'] for row in rows],
        'SV4GW': [row['gw4'] for row in rows],
        'SV6GW': [row['gw6'] for row in rows],
        'V4NET': [row['net4'] for row in rows],
        'V4HOST': [row['host4'] for row in rows],
        'V4LEN': [24] * len(rows),
        'V6PFX': [row['net6'] for row in rows],
        'V6HOST': [row['host6'] for row in rows],
        'V6LEN': [64] * len(rows),
        'HOST_MTU': [config['host_mtu']] * len(rows),
        'VLAN_MTU': [config['vlan_mtu']] * len(rows),
    }
    if config['qos_commands'] is not None:
        values['SW_QOS_CMDS'] = config['qos_commands']
    with env_path.open('a', encoding='utf-8') as target:
        for key, items in values.items():
            target.write(f'{key}=({" ".join(shlex.quote(str(item)) for item in items)})\n')
        target.write('PLAN_CONFIRMED=1\n')


class SwitchSession:
    def __init__(self, config, chassis, log_path):
        self.chassis = chassis
        prefix = ['sshpass', '-e'] if config.get('switch_password') else []
        env = os.environ.copy()
        if prefix:
            env['SSHPASS'] = config['switch_password']
        ssh_options = [] if prefix else ['-o', 'BatchMode=yes']
        command = prefix + [
            'ssh',
            '-tt',
            '-o',
            'ConnectTimeout=15',
            '-o',
            'StrictHostKeyChecking=accept-new',
            *ssh_options,
            '-o',
            'ServerAliveInterval=10',
            f'{config["switch_user"]}@{config["switch_host"]}',
        ]
        self.process = subprocess.Popen(
            command, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, bufsize=0
        )
        self.selector = selectors.DefaultSelector()
        self.selector.register(self.process.stdout, selectors.EVENT_READ)
        self.log = log_path.open('w', encoding='utf-8')
        self.prompt = re.compile(
            r'(?:<' + re.escape(chassis) + r'>|\[[~*]?' + re.escape(chassis) + r'[^\]\r\n]*\])\s*$'
        )
        try:
            self._receive('登录', 30)
            self.execute('screen-length 0 temporary')
        except Exception:
            self.close()
            raise

    def _receive(self, command, timeout):
        output = ''
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if not self.selector.select(max(0, deadline - time.monotonic())):
                break
            data = os.read(self.process.stdout.fileno(), 65536)
            if not data:
                fail(f'交换机连接中断: {command}; {output[-500:]}')
            output += data.decode('utf-8', errors='replace').replace('\r', '')
            if self.prompt.search(output):
                self.log.write(output)
                self.log.flush()
                return output
        fail(f'等待交换机回显超时: {command}; {output[-500:]}')

    def execute(self, command, allow_error=False):
        self.process.stdin.write((command + '\n').encode())
        self.process.stdin.flush()
        output = self._receive(command, 60)
        error_pattern = (
            r'(?im)^\s*(?:error\s*:|failure\s*:|failed\b|'
            r'%\s*(?:error|invalid|incomplete|unrecognized)|\^\s*$)'
        )
        if not allow_error and re.search(error_pattern, output):
            fail(f'交换机命令失败: {command}\n{output.strip()}')
        return output

    def close(self):
        if self.process.poll() is None:
            try:
                self.process.stdin.write(b'quit\n')
                self.process.stdin.flush()
            except BrokenPipeError:
                pass
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.terminate()
                self.process.wait(timeout=5)
        self.selector.close()
        self.process.stdin.close()
        self.process.stdout.close()
        self.log.close()


def gateway_addresses(config, family):
    if family == 4:
        entries = re.findall(r'(?m)^\s*ip address\s+(\S+)\s+(\S+)', config)
        return [str(ipaddress.ip_interface(f'{address}/{mask}')) for address, mask in entries]
    entries = re.findall(r'(?m)^\s*ipv6 address\s+(\S+)', config)
    return [str(ipaddress.ip_interface(address)) for address in entries]


def configure_network(config, out, deploy, rows, ports, nics, switch):
    host = {
        item['ifname']: item.get('addr_info', [])
        for item in json.loads((out / 'host-address.json').read_text(encoding='utf-8'))
    }
    families = (4, 6) if config['protocol'] == 'both' else (int(config['protocol'][1:]),)
    missing_host = []
    for family in families:
        present = []
        for nic, row in zip(nics, rows):
            addresses = [
                item
                for item in host.get(nic, [])
                if item.get('family') == f'inet{6 if family == 6 else ""}' and item.get('scope') == 'global'
            ]
            expected = row[f'host{family}']
            prefix = 24 if family == 4 else 64
            found = any(
                str(ipaddress.ip_address(item['local'])) == expected and item['prefixlen'] == prefix
                for item in addresses
            )
            if not found and addresses:
                fail(f'{nic} 已有其他 IPv{family} 地址，请先核对: {addresses}')
            present.append(found)
        if any(present) and not all(present):
            fail(f'IPv{family} 主机地址只有部分端口匹配，先核对或补齐')
        if not any(present):
            missing_host.append(family)

    snapshot = (out / 'switch-snapshot.txt').read_text(encoding='utf-8')
    missing_gateway = {family: set() for family in families}
    for row, port in zip(rows, ports):
        pattern = r'(?ms)^interface\s+' + re.escape(port) + r'\s*\n(.*?)(?=^#\s*$|^return\s*$)'
        match = re.search(pattern, snapshot)
        if not match:
            fail(f'交换机快照缺少端口 {port}')
        current_port = match.group(1)
        unsafe_port = (
            r'(?m)^\s*(?:undo portswitch|ip address\s+|ipv6 address\s+|'
            r'eth-trunk\s+|port link-type (?:trunk|hybrid))'
        )
        if re.search(unsafe_port, current_port):
            fail(f'{port} 包含三层地址或非 access 配置，请先核对端口归属')
        pvid = re.search(r'(?m)^\s*port default vlan\s+(\d+)', current_port)
        if pvid and int(pvid.group(1)) not in (1, row['vlan']):
            fail(f'{port} 已归属 VLAN {pvid.group(1)}，与规划 VLAN {row["vlan"]} 不一致')
        vlan = switch.execute(f'display vlan {row["vlan"]}', allow_error=True)
        if re.search(r'(?im)^\s*(?:Error|Failure)\s*:', vlan):
            fail(f'无法读取 VLAN {row["vlan"]}: {vlan.strip()}')
        members = set(re.findall(r'\b(?:\d+GE|GE|GigabitEthernet|XGigabitEthernet|Ethernet|Eth-Trunk)[\w/.:-]+', vlan))
        if members - {port}:
            fail(f'VLAN {row["vlan"]} 已被其他端口使用: {sorted(members)}')
        exists = re.search(r'(?m)^\s*' + str(row['vlan']) + r'\s+(?:UT:|TG:|common\b)', vlan)
        if exists:
            current = switch.execute(f'display current-configuration interface Vlanif {row["vlan"]}', allow_error=True)
            if re.search(r'(?im)^\s*(?:Error|Failure)\s*:', current):
                if 'Error: Wrong parameter found' not in current:
                    fail(f'无法读取 Vlanif {row["vlan"]}: {current.strip()}')
                current = ''
        else:
            current = ''
        for family in families:
            addresses = gateway_addresses(current, family)
            expected = str(ipaddress.ip_interface(f'{row[f"gw{family}"]}/{24 if family == 4 else 64}'))
            if addresses and expected not in addresses:
                fail(f'Vlanif {row["vlan"]} 已有不同的 IPv{family} 地址或前缀: {addresses}')
            if expected not in addresses:
                missing_gateway[family].add(row['vlan'])

    base = run(deploy + ['--switch', '--skip-ip'], capture=True)
    (out / 'switch-cmds.txt').write_text(base, encoding='utf-8')
    for command in base.splitlines():
        if command:
            switch.execute(command)
    added_gateway = False
    for family in families:
        targets = missing_gateway[family]
        if not targets:
            continue
        gateway = run(
            [
                'bash',
                str(SCRIPTS / '1825-deploy.sh'),
                '--env',
                str(out / 'env.sh'),
                '--protocol',
                f'v{family}',
                '--switch-ip',
            ],
            capture=True,
        )
        (out / f'switch-ip-v{family}.txt').write_text(gateway, encoding='utf-8')
        switch.execute('system-view')
        active = False
        for command in gateway.splitlines():
            match = re.fullmatch(r'interface Vlanif (\d+)', command)
            if match:
                active = int(match.group(1)) in targets
            if active:
                switch.execute(command)
            if command == 'quit':
                active = False
        switch.execute('return')
        added_gateway = True
    for row, port in zip(rows, ports):
        current = switch.execute(f'display current-configuration interface {port}')
        if not re.search(r'(?m)^\s*port default vlan\s+' + str(row['vlan']) + r'\s*$', current):
            fail(f'{port} 的 VLAN 未生效，请检查交换机回显')
        if not re.search(r'(?m)^\s*port bridge enable\s*$', current):
            fail(f'{port} 未启用端口桥，请检查交换机回显')
        current = switch.execute(f'display current-configuration interface Vlanif {row["vlan"]}')
        for family in families:
            expected = str(ipaddress.ip_interface(f'{row[f"gw{family}"]}/{24 if family == 4 else 64}'))
            if expected not in gateway_addresses(current, family):
                fail(f'Vlanif {row["vlan"]} 的 IPv{family} 网关或前缀未生效')
    for family in missing_host:
        run(
            [
                'bash',
                str(SCRIPTS / '1825-deploy.sh'),
                '--env',
                str(out / 'env.sh'),
                '--protocol',
                f'v{family}',
                '--ip',
                '--yes',
            ]
        )
    if added_gateway:
        print('新建网关，等待 20 秒让链路收敛。', flush=True)
        time.sleep(20)


def main():
    parser = argparse.ArgumentParser(description='根据预填配置自动配置 1825 主机与交换机')
    parser.add_argument('--config', required=True, help='JSON 配置文件')
    args = parser.parse_args()
    config = load_config(args.config)
    if os.geteuid() != 0:
        fail('请在宿主机使用 root 执行')
    out = Path(config['workspace']) / 'runs' / ('1825-auto-' + time.strftime('%Y%m%d-%H%M%S'))
    out.mkdir(parents=True, mode=0o700)
    print(f'运行目录: {out}', flush=True)
    env = os.environ.copy()
    collect = [
        sys.executable,
        str(SCRIPTS / '1825-collect.py'),
        '--out-dir',
        str(out),
        '--switch',
        config['switch_host'],
        '--switch-user',
        config['switch_user'],
    ]
    if config.get('switch_password'):
        env['MF_1825_SWITCH_PASSWORD'] = config['switch_password']
        collect += ['--ssh-password-env', 'MF_1825_SWITCH_PASSWORD']
    else:
        collect += ['--ssh-batch']
    run(collect, env=env)
    env_path = out / 'env.sh'
    ports = read_array(env_path, 'SPORT')
    nics = read_array(env_path, 'NIC')
    rows = read_plan(config, ports)
    write_plan(env_path, config, rows)
    deploy = ['bash', str(SCRIPTS / '1825-deploy.sh'), '--env', str(env_path), '--protocol', config['protocol']]
    run(deploy + ['--check'])
    snapshot = (out / 'switch-snapshot.txt').read_text(encoding='utf-8')
    match = re.search(r'(?m)^<([^>]+)>', snapshot)
    if not match:
        fail('交换机快照缺少系统名')
    switch = SwitchSession(config, match.group(1), out / 'switch-apply.log')
    try:
        configure_network(config, out, deploy, rows, ports, nics, switch)
    finally:
        switch.close()
    run(deploy + ['--host', '--skip-ip', '--yes'])
    run(deploy + ['--verify'])
    run(deploy + ['--hccn', '--family', config['run_family'], '--yes'])
    print(f'[OK] 交换机、主机网络和 hccn.conf 配置完成，记录: {out}', flush=True)


if __name__ == '__main__':
    try:
        main()
    except (RuntimeError, OSError, ValueError, subprocess.SubprocessError) as error:
        print(f'[ERROR] {error}', file=sys.stderr)
        sys.exit(1)
