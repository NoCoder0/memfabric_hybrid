#!/bin/bash
set -euo pipefail

ENV_FILE=/root/env.sh
MODE=
CONFIRM=0
SKIP_IP=0
FAMILY=
PAIR=0,1
REQUESTED_PROTO=${PROTO:-}
while [ "$#" -gt 0 ]; do
  case "$1" in
    --check|--switch|--switch-ip|--switch-clean|--ip|--host|--verify|--probe|--hccn|--rollback|--all) MODE=${1#--} ;;
    --env) shift; ENV_FILE=${1:?缺少环境文件路径} ;;
    --family) shift; FAMILY=${1:?缺少 ipv4 或 ipv6} ;;
    --protocol) shift; REQUESTED_PROTO=${1:?缺少 v4、v6 或 both} ;;
    --pair) shift; PAIR=${1:?缺少网卡数组下标，例如 0,1} ;;
    --skip-ip) SKIP_IP=1 ;;
    --yes) CONFIRM=1 ;;
    *) printf '[ERROR] 未知参数: %s\n' "$1" >&2; exit 2 ;;
  esac
  shift
done
[[ -n "$MODE" ]] || { echo '[ERROR] 指定 --check、--switch、--switch-ip、--switch-clean、--ip、--host、--verify、--probe、--hccn 或 --rollback' >&2; exit 2; }
[[ "$SKIP_IP" == 0 || "$MODE" == switch || "$MODE" == host ]] || { echo '[ERROR] --skip-ip 仅适用于 --switch 或 --host' >&2; exit 2; }
[[ "$MODE" != all ]] || { echo '[ERROR] 交换机命令需要人工执行；请按 --switch、--host、--verify 分步操作' >&2; exit 2; }
[[ -r "$ENV_FILE" ]] || { echo "[ERROR] 环境文件不可读: $ENV_FILE" >&2; exit 1; }
ENV_FILE=$(readlink -f "$ENV_FILE")
source "$ENV_FILE"
PROTO=${REQUESTED_PROTO:-${PROTO:-v6}}
case "$PROTO" in
  v4) FAMILIES=(-4) ;;
  v6) FAMILIES=(-6) ;;
  both) FAMILIES=(-4 -6) ;;
  *) echo '[ERROR] PROTO 只接受 v4、v6 或 both' >&2; exit 2 ;;
esac
if [[ -z "$FAMILY" ]]; then
  [[ "$PROTO" == v4 ]] && FAMILY=ipv4 || FAMILY=ipv6
fi
STATE_DIR="${ENV_FILE}.mf1825-state"
PLAN_HASH=$(sha256sum "$ENV_FILE" | awk '{print $1}')
SCRIPT_DIR=$(dirname "$(readlink -f "$0")")

fail() { printf '[ERROR] %s\n' "$*" >&2; exit 1; }
require_yes() { [[ "$CONFIRM" == 1 ]] || fail '该阶段会修改主机，请加 --yes'; }

validate() {
  [[ "${PLAN_CONFIRMED:-0}" == 1 ]] || fail '请核对采集值和手工规划后，将 PLAN_CONFIRMED 设置为 1'
  [[ -n "${MGT_IF:-}" ]] || fail 'MGT_IF 为空，请确认管理网络接口'
  local actual_mgt n i key
  actual_mgt=$(ip -4 route show default | awk 'NR==1 {for(i=1;i<=NF;i++) if($i=="dev") {print $(i+1);exit}}')
  [[ "$actual_mgt" == "$MGT_IF" ]] || fail "管理接口不匹配: 配置=$MGT_IF 实际=${actual_mgt:-未发现}"
  for key in SPORT NIC DEV SVLAN SV4GW SV6GW V4NET V4HOST V4LEN V6PFX V6HOST V6LEN TABLE HOST_MTU VLAN_MTU TRANSCEIVER SW_QOS_CMDS; do
    declare -p "$key" >/dev/null 2>&1 || fail "缺少数组 $key"
  done
  n=${#NIC[@]}
  (( n >= 2 )) || fail '至少需要两张数据面网卡'
  for key in SPORT DEV SVLAN SV4GW SV6GW V4NET V4HOST V4LEN V6PFX V6HOST V6LEN TABLE HOST_MTU VLAN_MTU TRANSCEIVER; do
    local -n values="$key"
    (( ${#values[@]} == n )) || fail "$key 数量 ${#values[@]} 与 NIC 数量 $n 不一致"
    unset -n values
  done
  (( ${#SW_QOS_CMDS[@]} > 0 )) || fail 'SW_QOS_CMDS 为空，请根据交换机配置或厂商规划填写'
  for i in "${!NIC[@]}"; do
    [[ "${NIC[$i]}" != "$MGT_IF" ]] || fail "数据面接口不能是管理口 $MGT_IF"
    [[ "${SPORT[$i]}" =~ ^[A-Za-z0-9][A-Za-z0-9/:.]*$ ]] || fail "交换机端口无效: ${SPORT[$i]}"
    [[ "${DEV[$i]}" =~ ^[0-9]+,[0-9]+$ ]] || fail "device 映射必须是两个数字: ${DEV[$i]}"
    [[ "${SVLAN[$i]}" =~ ^[0-9]+$ ]] && (( 10#${SVLAN[$i]} >= 2 && 10#${SVLAN[$i]} <= 4094 )) || fail "VLAN 无效: ${SVLAN[$i]}"
    [[ "${TABLE[$i]}" =~ ^[0-9]+$ ]] && (( 10#${TABLE[$i]} >= 1 && 10#${TABLE[$i]} <= 252 )) || fail "路由表号无效: ${TABLE[$i]}"
    [[ "${HOST_MTU[$i]}" =~ ^[0-9]+$ ]] && (( 10#${HOST_MTU[$i]} >= 576 && 10#${HOST_MTU[$i]} <= 65535 )) || fail "主机 MTU 无效: ${HOST_MTU[$i]}"
    [[ "${VLAN_MTU[$i]}" =~ ^[0-9]+$ ]] && (( 10#${VLAN_MTU[$i]} >= 576 && 10#${VLAN_MTU[$i]} <= 65535 )) || fail "Vlanif MTU 无效: ${VLAN_MTU[$i]}"
    [[ "${TRANSCEIVER[$i]}" =~ ^[A-Za-z0-9_-]*$ ]] || fail "光模块类型无效: ${TRANSCEIVER[$i]}"
    ip link show dev "${NIC[$i]}" >/dev/null || fail "网卡不存在: ${NIC[$i]}"
    [[ "$(ip -o link show dev "${NIC[$i]}")" != *" master $MGT_IF "* ]] || fail "${NIC[$i]} 属于管理接口 $MGT_IF"
  done
  for key in "${SW_QOS_CMDS[@]}"; do
    [[ "$key" =~ ^trust\ (dscp|8021p\ (inner|outer))$ || "$key" =~ ^(trust\ upstream\ |qos\ |dcb\ pfc\ )[A-Za-z0-9\ -]+$ ]] || fail "QoS 命令无效: $key"
  done
  python3 - "$PROTO" "$n" "${NIC[@]}" "${SPORT[@]}" "${DEV[@]}" "${SVLAN[@]}" "${TABLE[@]}" \
    "${V4NET[@]}" "${V4HOST[@]}" "${SV4GW[@]}" "${V4LEN[@]}" \
    "${V6PFX[@]}" "${V6HOST[@]}" "${SV6GW[@]}" "${V6LEN[@]}" <<'PY'
import ipaddress, sys
protocol = sys.argv[1]; n = int(sys.argv[2]); args = sys.argv[3:]
groups = [args[i*n:(i+1)*n] for i in range(13)]
nic, port, dev, vlan, table, net4, host4, gw4, len4, net6, host6, gw6, len6 = groups
for name, values in [('NIC', nic), ('SPORT', port), ('SVLAN', vlan), ('TABLE', table)]:
    if len(set(values)) != n:
        raise SystemExit(f'[ERROR] {name} 存在重复值')
devices = [item for pair in dev for item in pair.split(',')]
if len(set(devices)) != len(devices):
    raise SystemExit('[ERROR] device 映射存在重复值')
for family, networks, hosts, gateways, lengths in [(4, net4, host4, gw4, len4), (6, net6, host6, gw6, len6)]:
    if (family == 4 and protocol == 'v6') or (family == 6 and protocol == 'v4'):
        continue
    seen = []
    try:
        for i in range(n):
            network = ipaddress.ip_network(f'{networks[i]}/{lengths[i]}', strict=True)
            host = ipaddress.ip_address(hosts[i]); gateway = ipaddress.ip_address(gateways[i])
            if network.version != family or host not in network or gateway not in network or host == gateway:
                raise ValueError(f'第 {i} 口主机或网关不在网段内，或地址相同')
            if family == 4 and (host in (network.network_address, network.broadcast_address) or
                                gateway in (network.network_address, network.broadcast_address)):
                raise ValueError(f'第 {i} 口使用了 IPv4 网络地址或广播地址')
            if any(network.overlaps(other) for other in seen):
                raise ValueError(f'第 {i} 口与其他网段重叠')
            seen.append(network)
    except ValueError as error:
        raise SystemExit(f'[ERROR] IPv{family} 地址规划无效: {error}')
PY
  if [[ "$PROTO" != v4 || ( "$MODE" == rollback && -e "$STATE_DIR/active-6" ) ]]; then
    for i in "${!NIC[@]}"; do
      V6HOST[$i]=$(python3 -c 'import ipaddress,sys; print(ipaddress.ip_address(sys.argv[1]))' "${V6HOST[$i]}")
      SV6GW[$i]=$(python3 -c 'import ipaddress,sys; print(ipaddress.ip_address(sys.argv[1]))' "${SV6GW[$i]}")
      V6PFX[$i]=$(python3 -c 'import ipaddress,sys; print(ipaddress.ip_address(sys.argv[1]))' "${V6PFX[$i]}")
    done
  fi
}

rule_exists() {
  local family=$1 priority=$2 src=$3 dst=$4 table=$5
  ip "$family" rule show | awk -v p="$priority:" -v s="$src" -v d="$dst" -v t="$table" '
    d == "" && $1 == p && $2 == "from" && $3 == s && $4 == "lookup" && $5 == t && NF == 5 { found=1 }
    d != "" && $1 == p && $2 == "from" && $3 == s && $4 == "to" && $5 == d &&
      $6 == "iif" && $7 == "lo" && $8 == "lookup" && $9 == t && NF == 9 { found=1 }
    END { exit !found }'
}

local_rule_exists() {
  ip "$1" rule show | grep -Eq "^$2:[[:space:]]+from all lookup local([[:space:]]|$)"
}

route_exists() {
  local family=$1 table=$2 destination=$3 gateway=$4 device=$5 source=$6 routes
  if ! routes=$(ip "$family" route show table "$table" 2>&1); then
    [[ "$routes" == *'FIB table does not exist'* ]] && return 1
    fail "无法读取 $family 路由表 $table: $routes"
  fi
  awk -v dest="$destination" -v gw="$gateway" -v dev="$device" -v src="$source" '
    $1 == dest {
      g=(gw == ""); d=0; s=(src == "")
      for (i=2; i<NF; i++) {
        if ($i == "via" && $(i+1) == gw) g=1
        if ($i == "dev" && $(i+1) == dev) d=1
        if ($i == "src" && $(i+1) == src) s=1
      }
      if (g && d && s) found=1
    }
    END { exit !found }' <<< "$routes"
}

address_exists() {
  ip -o "$1" address show dev "$2" | awk '{print $4}' | grep -Fxq "$3"
}

check_empty_tables() {
  local families=$1
  python3 - "$families" "${TABLE[@]}" <<'PY'
import json, pathlib, re, subprocess, sys
families = ('-4', '-6') if sys.argv[1] == 'both' else (sys.argv[1],)
tables = set(sys.argv[2:]); aliases = {}
paths = [pathlib.Path('/etc/iproute2/rt_tables')]
paths.extend(pathlib.Path('/etc/iproute2/rt_tables.d').glob('*.conf'))
for path in paths:
    if path.is_file():
        for line in path.read_text(errors='replace').splitlines():
            match = re.match(r'^\s*(\d+)\s+(\S+)', line)
            if match:
                aliases[match.group(2)] = match.group(1)
                if match.group(1) in tables:
                    raise SystemExit(f'[ERROR] table {match.group(1)} 已在 {path} 注册')
for family in families:
    for kind in ('rule', 'route'):
        command = ['ip', '-j', family, kind, 'show']
        if kind == 'route': command += ['table', 'all']
        try:
            entries = json.loads(subprocess.run(command, check=True, capture_output=True, text=True).stdout)
        except (ValueError, subprocess.CalledProcessError) as error:
            raise SystemExit(f'[ERROR] 无法检查路由表占用: {error}')
        for entry in entries:
            name = str(entry.get('table', ''))
            if aliases.get(name, name) in tables:
                raise SystemExit(f'[ERROR] {family} table {name} 已被 {kind} 引用或占用')
PY
}

check_target_addresses() {
  local family=$1 i ipaddr
  for i in "${!NIC[@]}"; do
    [[ "$family" == -4 ]] && ipaddr=${V4HOST[$i]} || ipaddr=${V6HOST[$i]}
    if ip -o "$family" address show | awk '{print $4}' | cut -d/ -f1 | grep -Fxq "$ipaddr"; then
      fail "$ipaddr 已存在；先清理已确认属于本方案的地址，不能覆盖未知配置"
    fi
  done
}

check_existing_addresses() {
  local i f expected
  for f in "${FAMILIES[@]}"; do
    for i in "${!NIC[@]}"; do
      if [[ "$f" == -4 ]]; then expected="${V4HOST[$i]}/${V4LEN[$i]}"; else expected="${V6HOST[$i]}/${V6LEN[$i]}"; fi
      address_exists "$f" "${NIC[$i]}" "$expected" || fail "$f ${NIC[$i]} 缺少规划地址 $expected，不能跳过 IP 配置"
      local matches
      matches=$(ip -o "$f" address show | awk -v address="${expected%/*}" '{split($4, value, "/"); if (value[1] == address) count++} END {print count+0}')
      [[ "$matches" == 1 ]] || fail "$f $expected 在本机出现 $matches 次，不能跳过 IP 配置"
    done
  done
}

check_conflicting_rules() {
  python3 - "$PROTO" "${#NIC[@]}" "${V4HOST[@]}" "${V6HOST[@]}" "${TABLE[@]}" <<'PY'
import ipaddress, json, subprocess, sys
protocol, count = sys.argv[1], int(sys.argv[2])
values = sys.argv[3:]
v4, v6, tables = (values[i * count:(i + 1) * count] for i in range(3))
for family, addresses in ((4, v4), (6, v6)):
    if (family == 4 and protocol == 'v6') or (family == 6 and protocol == 'v4'):
        continue
    rules = json.loads(subprocess.run(['ip', '-j', f'-{family}', 'rule', 'show'],
                                      check=True, capture_output=True, text=True).stdout)
    planned = {str(ipaddress.ip_address(addr)): table for addr, table in zip(addresses, tables)}
    for rule in rules:
        try:
            source = str(ipaddress.ip_interface(rule.get('src', '')).ip)
        except ValueError:
            continue
        if source not in planned:
            continue
        priority, table = rule.get('priority'), str(rule.get('table', ''))
        destination = rule.get('dst', '')
        if priority == 0 and rule.get('iif') == 'lo':
            try:
                destination = str(ipaddress.ip_interface(destination).ip)
            except ValueError:
                continue
            if destination not in planned or destination == source:
                continue
        elif priority != 200 or destination not in ('', 'all'):
            continue
        if table != planned[source]:
            raise SystemExit(f'[ERROR] IPv{family} {source} 的现有规则使用 table {table}，规划为 '
                             f'{planned[source]}；请恢复原环境文件或核对规则，未修改网络')
PY
}

discover_existing_routes() {
  python3 - "$PROTO" "${#NIC[@]}" "${NIC[@]}" "${V4HOST[@]}" "${V6HOST[@]}" \
    "${V4NET[@]}" "${V6PFX[@]}" "${V4LEN[@]}" "${V6LEN[@]}" "${SV4GW[@]}" "${SV6GW[@]}" <<'PY'
import ipaddress, json, subprocess, sys
protocol, count = sys.argv[1], int(sys.argv[2])
args = sys.argv[3:]
nics, host4, host6, net4, net6, len4, len6, gw4, gw6 = (
    args[i * count:(i + 1) * count] for i in range(9))

def address(value):
    try:
        return str(ipaddress.ip_interface(value).ip)
    except ValueError:
        return ''

def read_ip(*command):
    return json.loads(subprocess.run(['ip', '-j', *command], check=True, capture_output=True, text=True).stdout)

families = []
for family, hosts, networks, lengths, gateways in ((4, host4, net4, len4, gw4), (6, host6, net6, len6, gw6)):
    if (family == 4 and protocol == 'v6') or (family == 6 and protocol == 'v4'):
        continue
    rules = read_ip(f'-{family}', 'rule', 'show')
    hosts = [address(host) for host in hosts]
    families.append((family, hosts, networks, lengths, gateways, rules))
found = any(rule.get('priority') == 0 and rule.get('iif') == 'lo'
            and address(rule.get('src', '')) in hosts and address(rule.get('dst', '')) in hosts
            for _, hosts, _, _, _, rules in families for rule in rules)
if not found:
    raise SystemExit(3)
selected = None
for family, hosts, networks, lengths, gateways, rules in families:
    local0 = any(rule.get('priority', 0) == 0 and rule.get('table') == 'local' for rule in rules)
    local1 = any(rule.get('priority') == 1 and rule.get('table') == 'local' for rule in rules)
    if local0 or not local1:
        raise SystemExit(f'[ERROR] IPv{family} 已有规则的 local 优先级不符合要求，未修改网络')
    tables = []
    for i, source in enumerate(hosts):
        values = set()
        for destination in hosts:
            if destination == source:
                continue
            matches = [str(rule.get('table', '')) for rule in rules if rule.get('priority') == 0
                       and rule.get('iif') == 'lo' and address(rule.get('src', '')) == source
                       and address(rule.get('dst', '')) == destination]
            if len(matches) != 1:
                raise SystemExit(f'[ERROR] IPv{family} {source} -> {destination} 的现有规则不完整或重复，未修改网络')
            values.add(matches[0])
        if len(values) != 1:
            raise SystemExit(f'[ERROR] IPv{family} {source} 的现有规则使用多个路由表，未修改网络')
        table = values.pop()
        if not table.isdigit() or not 1 <= int(table) <= 252:
            raise SystemExit(f'[ERROR] IPv{family} {source} 的现有路由表无效: {table}')
        if not any(rule.get('priority') == 200 and address(rule.get('src', '')) == source
                   and str(rule.get('table', '')) == table for rule in rules):
            raise SystemExit(f'[ERROR] IPv{family} {source} 缺少 priority 200 规则，未修改网络')
        routes = read_ip(f'-{family}', 'route', 'show', 'table', table)
        network = str(ipaddress.ip_network(f'{networks[i]}/{lengths[i]}', strict=True))
        gateway = str(ipaddress.ip_address(gateways[i]))
        connected = any(route.get('dst') == network and route.get('dev') == nics[i]
                        and address(route.get('prefsrc', '')) == source for route in routes)
        default = any(route.get('dst') == 'default' and route.get('dev') == nics[i]
                      and address(route.get('gateway', '')) == gateway for route in routes)
        if not connected or not default:
            raise SystemExit(f'[ERROR] IPv{family} {source} 的 table {table} 缺少规划路由，未修改网络')
        tables.append(table)
    if len(set(tables)) != len(tables) or (selected is not None and selected != tables):
        raise SystemExit('[ERROR] 现有双栈路由表不一致或重复，未修改网络')
    selected = tables
print(' '.join(selected))
PY
}

show_plan() {
  local i
  printf '网卡 | 交换机口 | device | VLAN | IPv4 网关 | IPv6 网关 | 表\n' >&2
  for i in "${!NIC[@]}"; do
    printf '%s | %s | %s | %s | %s | %s | %s\n' "${NIC[$i]}" "${SPORT[$i]}" "${DEV[$i]}" \
      "${SVLAN[$i]}" "${SV4GW[$i]}" "${SV6GW[$i]}" "${TABLE[$i]}" >&2
  done
}

switch_commands() {
  local i cmd
  printf 'system-view\n'
  for i in "${!NIC[@]}"; do
    printf 'vlan batch %s\ncommit\ninterface %s\nportswitch\nport link-type access\nport default vlan %s\nport bridge enable\n' \
      "${SVLAN[$i]}" "${SPORT[$i]}" "${SVLAN[$i]}"
    for cmd in "${SW_QOS_CMDS[@]}"; do printf '%s\n' "$cmd"; done
    [[ -z "${TRANSCEIVER[$i]}" ]] || printf 'device transceiver %s\n' "${TRANSCEIVER[$i]}"
    printf 'undo shutdown\ncommit\nquit\ninterface Vlanif %s\nmtu %s\n' "${SVLAN[$i]}" "${VLAN_MTU[$i]}"
    if [[ "$PROTO" != v4 ]]; then printf 'ipv6 enable\n'; fi
    if [[ "$PROTO" != v6 && "$SKIP_IP" == 0 ]]; then
      printf 'ip address %s %s\n' "${SV4GW[$i]}" \
        "$(python3 -c 'import ipaddress,sys; print(ipaddress.IPv4Network("0.0.0.0/"+sys.argv[1]).netmask)' "${V4LEN[$i]}")"
    fi
    if [[ "$PROTO" != v4 && "$SKIP_IP" == 0 ]]; then printf 'ipv6 address %s/%s\n' "${SV6GW[$i]}" "${V6LEN[$i]}"; fi
    printf 'commit\nquit\n'
  done
  printf 'return\n'
}

switch_ip_commands() {
  local i
  printf 'system-view\n'
  for i in "${!NIC[@]}"; do
    printf 'interface Vlanif %s\n' "${SVLAN[$i]}"
    if [[ "$PROTO" != v6 ]]; then
      printf 'ip address %s %s\n' "${SV4GW[$i]}" \
        "$(python3 -c 'import ipaddress,sys; print(ipaddress.IPv4Network("0.0.0.0/"+sys.argv[1]).netmask)' "${V4LEN[$i]}")"
    fi
    if [[ "$PROTO" != v4 ]]; then printf 'ipv6 address %s/%s\n' "${SV6GW[$i]}" "${V6LEN[$i]}"; fi
    printf 'commit\nquit\n'
  done
  printf 'return\n'
}

switch_cleanup_commands() {
  local i
  printf 'system-view\n'
  for i in "${!NIC[@]}"; do
    printf 'interface %s\nport default vlan 1\nundo port bridge enable\ncommit\nquit\n' "${SPORT[$i]}"
  done
  for i in "${!NIC[@]}"; do
    printf 'undo interface Vlanif %s\ncommit\nundo vlan %s\ncommit\n' "${SVLAN[$i]}" "${SVLAN[$i]}"
  done
  printf 'return\n'
}

save_initial_state() {
  if [[ -d "$STATE_DIR" ]]; then
    [[ -r "$STATE_DIR/plan.sha256" && "$(cat "$STATE_DIR/plan.sha256")" == "$PLAN_HASH" ]] || fail "状态目录与环境文件不匹配: $STATE_DIR"
    return
  fi
  check_empty_tables both
  local i f ipaddr count
  for f in "${FAMILIES[@]}"; do
    for count in 0 1; do
      local entries
      entries=$(ip "$f" rule show | grep -Ec "^$count:[[:space:]]+from all lookup local([[:space:]]|$)" || true)
      (( entries <= 1 )) || fail "$f priority $count 存在多个 local 规则，停止配置"
    done
  done
  if [[ "$SKIP_IP" == 1 ]]; then check_existing_addresses; else
    for f in "${FAMILIES[@]}"; do check_target_addresses "$f"; done
  fi
  mkdir -m 700 "$STATE_DIR"
  printf '%s\n' "$PLAN_HASH" > "$STATE_DIR/plan.sha256"
  for f in -4 -6; do
    local_rule_exists "$f" 0 && echo 1 > "$STATE_DIR/local0${f}" || echo 0 > "$STATE_DIR/local0${f}"
    local_rule_exists "$f" 1 && echo 1 > "$STATE_DIR/local1${f}" || echo 0 > "$STATE_DIR/local1${f}"
  done
  for i in "${!NIC[@]}"; do
    for f in -4 -6; do
      if [[ "$f" == -4 ]]; then ipaddr="${V4HOST[$i]}/${V4LEN[$i]}"; else ipaddr="${V6HOST[$i]}/${V6LEN[$i]}"; fi
      if [[ -n "${ipaddr%/*}" ]] && address_exists "$f" "${NIC[$i]}" "$ipaddr"; then
        echo 0 > "$STATE_DIR/ip-owned${f}.$i"
      else
        echo 1 > "$STATE_DIR/ip-owned${f}.$i"
      fi
    done
    ip -o link show dev "${NIC[$i]}" | awk '{for(i=1;i<=NF;i++) if($i=="mtu") {print $(i+1);exit}}' > "$STATE_DIR/mtu.$i"
    sysctl -n "net.ipv6.conf.${NIC[$i]}.accept_ra" > "$STATE_DIR/ra.$i"
    sysctl -n "net.ipv6.conf.${NIC[$i]}.autoconf" > "$STATE_DIR/autoconf.$i"
    ip -o link show dev "${NIC[$i]}" | grep -Eq '<[^>]*\bUP\b' && echo 1 > "$STATE_DIR/up.$i" || echo 0 > "$STATE_DIR/up.$i"
  done
}

ip_config() {
  require_yes
  save_initial_state
  local f i
  for f in "${FAMILIES[@]}"; do
    if [[ ! -e "$STATE_DIR/ip-active${f}" ]]; then
      check_target_addresses "$f"
      touch "$STATE_DIR/ip-active${f}"
    fi
  done
  for i in "${!NIC[@]}"; do
    ip link set dev "${NIC[$i]}" up mtu "${HOST_MTU[$i]}"
    if [[ "$PROTO" != v6 ]]; then ip -4 addr replace "${V4HOST[$i]}/${V4LEN[$i]}" dev "${NIC[$i]}"; fi
    if [[ "$PROTO" != v4 ]]; then
      sysctl -qw "net.ipv6.conf.${NIC[$i]}.accept_ra=0"
      sysctl -qw "net.ipv6.conf.${NIC[$i]}.autoconf=0"
      ip -6 addr replace "${V6HOST[$i]}/${V6LEN[$i]}" dev "${NIC[$i]}" nodad
    fi
  done
  check_existing_addresses
  echo "[OK] $PROTO 规划 IP 地址已配置" >&2
}

host_config() {
  require_yes
  if [[ -d "$STATE_DIR" ]]; then
    [[ -r "$STATE_DIR/plan.sha256" && "$(cat "$STATE_DIR/plan.sha256")" == "$PLAN_HASH" ]] || fail "状态目录与环境文件不匹配: $STATE_DIR"
  fi
  if [[ "$SKIP_IP" == 1 ]]; then
    check_existing_addresses
    if [[ ! -d "$STATE_DIR" ]]; then
      local reused status
      if reused=$(discover_existing_routes); then
        read -r -a TABLE <<< "$reused"
        mkdir -m 700 "$STATE_DIR"
        printf '%s\n' "$PLAN_HASH" > "$STATE_DIR/plan.sha256"
        printf '%s\n' "$reused" > "$STATE_DIR/reused-tables"
        touch "$STATE_DIR/reuse-routing"
        for f in "${FAMILIES[@]}"; do touch "$STATE_DIR/active${f}"; done
        verify_rules
        echo '[OK] 已复用现有 IP、路由和规则；未修改网络' >&2
        return
      else
        status=$?
        [[ "$status" == 3 ]] || return "$status"
      fi
    fi
    check_conflicting_rules
  fi
  if [[ -e "$STATE_DIR/reuse-routing" ]]; then
    [[ "$SKIP_IP" == 1 ]] || fail '现有路由已复用；请使用 --skip-ip'
    local current_routes
    current_routes=$(discover_existing_routes) || fail '复用的路由或规则已变化，请核对现网'
    [[ "$current_routes" == "${TABLE[*]}" ]] || fail '复用的路由表已变化，请核对现网'
    verify_rules
    echo '[OK] 已复用现有 IP、路由和规则；未修改网络' >&2
    return
  fi
  save_initial_state
  local i j f src dst
  for f in "${FAMILIES[@]}"; do
    if [[ ! -e "$STATE_DIR/active${f}" ]]; then
      check_empty_tables "$f"
      [[ "$SKIP_IP" == 1 ]] || check_target_addresses "$f"
      touch "$STATE_DIR/active${f}"
    fi
  done
  for i in "${!NIC[@]}"; do
    ip link set dev "${NIC[$i]}" up mtu "${HOST_MTU[$i]}"
    if [[ "$PROTO" != v6 ]]; then
      if [[ "$SKIP_IP" == 0 ]]; then ip -4 addr replace "${V4HOST[$i]}/${V4LEN[$i]}" dev "${NIC[$i]}"; fi
      ip -4 route replace "${V4NET[$i]}/${V4LEN[$i]}" dev "${NIC[$i]}" src "${V4HOST[$i]}" table "${TABLE[$i]}"
      ip -4 route replace default via "${SV4GW[$i]}" dev "${NIC[$i]}" table "${TABLE[$i]}"
    fi
    if [[ "$PROTO" != v4 ]]; then
      sysctl -qw "net.ipv6.conf.${NIC[$i]}.accept_ra=0"
      sysctl -qw "net.ipv6.conf.${NIC[$i]}.autoconf=0"
      if [[ "$SKIP_IP" == 0 ]]; then ip -6 addr replace "${V6HOST[$i]}/${V6LEN[$i]}" dev "${NIC[$i]}" nodad; fi
      ip -6 route replace "${V6PFX[$i]}/${V6LEN[$i]}" dev "${NIC[$i]}" src "${V6HOST[$i]}" table "${TABLE[$i]}"
      ip -6 route replace default via "${SV6GW[$i]}" dev "${NIC[$i]}" table "${TABLE[$i]}"
    fi
    for f in "${FAMILIES[@]}"; do
      [[ "$f" == -4 ]] && src=${V4HOST[$i]} || src=${V6HOST[$i]}
      rule_exists "$f" 200 "$src" '' "${TABLE[$i]}" || ip "$f" rule add priority 200 from "$src" lookup "${TABLE[$i]}"
    done
  done
  for f in "${FAMILIES[@]}"; do local_rule_exists "$f" 1 || ip "$f" rule add priority 1 lookup local; done
  for i in "${!NIC[@]}"; do
    for j in "${!NIC[@]}"; do
      [[ "$i" != "$j" ]] || continue
      for f in "${FAMILIES[@]}"; do
        if [[ "$f" == -4 ]]; then src=${V4HOST[$i]}; dst=${V4HOST[$j]}; else src=${V6HOST[$i]}; dst=${V6HOST[$j]}; fi
        rule_exists "$f" 0 "$src" "$dst" "${TABLE[$i]}" || ip "$f" rule add priority 0 from "$src" to "$dst" iif lo lookup "${TABLE[$i]}"
      done
    done
  done
  for f in "${FAMILIES[@]}"; do
    local_rule_exists "$f" 1 || fail "$f priority 1 local 缺失"
    local_rule_exists "$f" 0 && ip "$f" rule del priority 0 lookup local
    local_rule_exists "$f" 0 && fail "$f priority 0 local 仍然存在"
  done
  verify_rules
  if [[ "$SKIP_IP" == 1 ]]; then
    echo '[OK] 已核对现有地址，并配置路由和精确规则' >&2
  else
    echo '[OK] 主机地址、路由和精确规则已配置' >&2
  fi
}

verify_rules() {
  local i j f src dst
  for f in "${FAMILIES[@]}"; do
    local_rule_exists "$f" 1 || fail "$f priority 1 local 缺失"
    local_rule_exists "$f" 0 && fail "$f priority 0 local 仍然存在"
    for i in "${!NIC[@]}"; do
      for j in "${!NIC[@]}"; do
        [[ "$i" != "$j" ]] || continue
        if [[ "$f" == -4 ]]; then src=${V4HOST[$i]}; dst=${V4HOST[$j]}; else src=${V6HOST[$i]}; dst=${V6HOST[$j]}; fi
        rule_exists "$f" 0 "$src" "$dst" "${TABLE[$i]}" || fail "$f 缺少 $src -> $dst table ${TABLE[$i]} 的 priority 0 规则"
      done
    done
  done
  echo "[OK] $PROTO local 和每族 $((${#NIC[@]} * (${#NIC[@]} - 1))) 条精确规则通过" >&2
}

write_hccn() {
  require_yes
  [[ -d "$STATE_DIR" && -r "$STATE_DIR/plan.sha256" && "$(cat "$STATE_DIR/plan.sha256")" == "$PLAN_HASH" ]] || fail '先执行 --host，或状态文件与环境文件不匹配'
  [[ "$FAMILY" == ipv4 || "$FAMILY" == ipv6 ]] || fail '--family 只接受 ipv4 或 ipv6'
  if [[ "$FAMILY" == ipv4 ]]; then
    [[ -e "$STATE_DIR/active-4" ]] || fail 'IPv4 尚未通过 --host 配置'
  else
    [[ -e "$STATE_DIR/active-6" ]] || fail 'IPv6 尚未通过 --host 配置'
  fi
  if [[ ! -e "$STATE_DIR/hccn.saved" ]]; then
    if [[ -e /etc/hccn.conf ]]; then cp -p /etc/hccn.conf "$STATE_DIR/hccn.before"; echo 1 > "$STATE_DIR/hccn.saved"; else echo 0 > "$STATE_DIR/hccn.saved"; fi
  fi
  local data=() i
  for i in "${!NIC[@]}"; do
    if [[ "$FAMILY" == ipv4 ]]; then data+=("${DEV[$i]}=${V4HOST[$i]}"); else data+=("${DEV[$i]}=${V6HOST[$i]}"); fi
  done
  python3 - "$FAMILY" "${data[@]}" <<'PY'
from pathlib import Path
import os, sys
path = Path('/etc/hccn.conf'); values = {}; family = sys.argv[1]
for item in sys.argv[2:]:
    pair, address = item.split('=', 1)
    for device in pair.split(','): values['address_' + device] = address
if not path.exists(): path.touch(mode=0o640)
lines = [line for line in path.read_text().splitlines() if line.partition('=')[0].strip() not in values]
lines.extend(f'{key}={value}' for key, value in values.items())
with path.open('r+b') as stream:
    stream.seek(0); stream.write(('\n'.join(lines) + '\n').encode()); stream.truncate(); stream.flush(); os.fsync(stream.fileno())
print(f'[OK] {path}: {len(values)} 个 device 已配置，地址族 {family}')
PY
}

rollback() {
  require_yes
  [[ -d "$STATE_DIR" && -r "$STATE_DIR/plan.sha256" && "$(cat "$STATE_DIR/plan.sha256")" == "$PLAN_HASH" ]] || fail '状态目录不存在或环境文件已改变；禁止宽范围清理'
  local f i j src dst reused=0
  [[ ! -e "$STATE_DIR/reuse-routing" ]] || reused=1
  if [[ "$reused" == 0 ]]; then
  for f in -4 -6; do
    [[ -e "$STATE_DIR/active${f}" ]] || continue
    if [[ "$(cat "$STATE_DIR/local0${f}")" == 1 ]]; then local_rule_exists "$f" 0 || ip "$f" rule add priority 0 lookup local; fi
    for i in "${!NIC[@]}"; do
      for j in "${!NIC[@]}"; do
        [[ "$i" != "$j" ]] || continue
        if [[ "$f" == -4 ]]; then src=${V4HOST[$i]}; dst=${V4HOST[$j]}; else src=${V6HOST[$i]}; dst=${V6HOST[$j]}; fi
        while rule_exists "$f" 0 "$src" "$dst" "${TABLE[$i]}"; do ip "$f" rule del priority 0 from "$src" to "$dst" iif lo lookup "${TABLE[$i]}"; done
      done
      [[ "$f" == -4 ]] && src=${V4HOST[$i]} || src=${V6HOST[$i]}
      while rule_exists "$f" 200 "$src" '' "${TABLE[$i]}"; do ip "$f" rule del priority 200 from "$src" lookup "${TABLE[$i]}"; done
    done
    if [[ "$(cat "$STATE_DIR/local1${f}")" == 0 ]] && local_rule_exists "$f" 1; then ip "$f" rule del priority 1 lookup local; fi
  done
  for i in "${!NIC[@]}"; do
    if [[ -e "$STATE_DIR/active-4" ]] && route_exists -4 "${TABLE[$i]}" default "${SV4GW[$i]}" "${NIC[$i]}" ''; then
      ip -4 route del default via "${SV4GW[$i]}" dev "${NIC[$i]}" table "${TABLE[$i]}"
    fi
    if [[ -e "$STATE_DIR/active-4" ]] && route_exists -4 "${TABLE[$i]}" "${V4NET[$i]}/${V4LEN[$i]}" '' "${NIC[$i]}" "${V4HOST[$i]}"; then
      ip -4 route del "${V4NET[$i]}/${V4LEN[$i]}" dev "${NIC[$i]}" table "${TABLE[$i]}"
    fi
    if [[ -e "$STATE_DIR/active-6" ]] && route_exists -6 "${TABLE[$i]}" default "${SV6GW[$i]}" "${NIC[$i]}" ''; then
      ip -6 route del default via "${SV6GW[$i]}" dev "${NIC[$i]}" table "${TABLE[$i]}"
    fi
    if [[ -e "$STATE_DIR/active-6" ]] && route_exists -6 "${TABLE[$i]}" "${V6PFX[$i]}/${V6LEN[$i]}" '' "${NIC[$i]}" "${V6HOST[$i]}"; then
      ip -6 route del "${V6PFX[$i]}/${V6LEN[$i]}" dev "${NIC[$i]}" table "${TABLE[$i]}"
    fi
    if [[ ( -e "$STATE_DIR/active-4" || -e "$STATE_DIR/ip-active-4" ) && ( ! -e "$STATE_DIR/ip-owned-4.$i" || "$(cat "$STATE_DIR/ip-owned-4.$i")" == 1 ) ]] && address_exists -4 "${NIC[$i]}" "${V4HOST[$i]}/${V4LEN[$i]}"; then
      ip -4 addr del "${V4HOST[$i]}/${V4LEN[$i]}" dev "${NIC[$i]}"
    fi
    if [[ ( -e "$STATE_DIR/active-6" || -e "$STATE_DIR/ip-active-6" ) && ( ! -e "$STATE_DIR/ip-owned-6.$i" || "$(cat "$STATE_DIR/ip-owned-6.$i")" == 1 ) ]] && address_exists -6 "${NIC[$i]}" "${V6HOST[$i]}/${V6LEN[$i]}"; then
      ip -6 addr del "${V6HOST[$i]}/${V6LEN[$i]}" dev "${NIC[$i]}"
    fi
    if [[ -e "$STATE_DIR/active-6" || -e "$STATE_DIR/ip-active-6" ]]; then
      sysctl -qw "net.ipv6.conf.${NIC[$i]}.accept_ra=$(cat "$STATE_DIR/ra.$i")"
      sysctl -qw "net.ipv6.conf.${NIC[$i]}.autoconf=$(cat "$STATE_DIR/autoconf.$i")"
    fi
    ip link set dev "${NIC[$i]}" mtu "$(cat "$STATE_DIR/mtu.$i")"
    [[ "$(cat "$STATE_DIR/up.$i")" == 1 ]] || ip link set dev "${NIC[$i]}" down
  done
  fi
  if [[ -e "$STATE_DIR/hccn.saved" ]]; then
    python3 - "$STATE_DIR" "${DEV[@]}" <<'PY'
from pathlib import Path
import os, sys
state = Path(sys.argv[1]); path = Path('/etc/hccn.conf')
keys = {'address_' + device for pair in sys.argv[2:] for device in pair.split(',')}
before = (state / 'hccn.before').read_text() if (state / 'hccn.saved').read_text().strip() == '1' else ''
current = path.read_text()
keep = lambda lines: [line for line in lines if line.partition('=')[0].strip() not in keys]
if keep(current.splitlines()) == keep(before.splitlines()):
    data = before.encode()
else:
    previous = [line for line in before.splitlines() if line.partition('=')[0].strip() in keys]
    merged = keep(current.splitlines()) + previous
    data = ('\n'.join(merged) + ('\n' if merged else '')).encode()
with path.open('r+b') as stream:
    stream.seek(0); stream.write(data); stream.truncate(); stream.flush(); os.fsync(stream.fileno())
PY
  fi
  mv "$STATE_DIR" "${STATE_DIR}.completed.$(date +%Y%m%d%H%M%S)"
  if [[ "$reused" == 1 ]]; then
    echo '[OK] 已保留原有 IP、路由和规则，并按状态恢复 hccn' >&2
  else
    echo '[OK] 已按状态文件清理本方案的地址、路由和规则，并恢复主机参数' >&2
  fi
}

if [[ -r "$STATE_DIR/reused-tables" ]]; then
  read -r -a TABLE < "$STATE_DIR/reused-tables"
  [[ ${#TABLE[@]} -eq ${#NIC[@]} ]] || fail '已复用的路由表数量不匹配'
fi
validate
case "$MODE" in
  check) show_plan ;;
  switch) switch_commands ;;
  switch-ip) switch_ip_commands ;;
  switch-clean) switch_cleanup_commands ;;
  ip) ip_config ;;
  host) host_config ;;
  verify)
    verify_rules
    if [[ "$PROTO" != v4 ]]; then
      route=$(ip -6 route get "${V6HOST[1]}" from "${V6HOST[0]}" iif lo)
      echo "$route"
      [[ "$route" == *"via ${SV6GW[0]}"* && "$route" == *"dev ${NIC[0]}"* && "$route" == *"table ${TABLE[0]}"* ]] || fail 'IPv6 模拟输入路由未命中预期网关、网卡和表'
    else
      echo '[OK] IPv4 规则已检查；请用 --probe 验证真实出入网卡' >&2
    fi
    ;;
  probe)
    [[ "$PAIR" =~ ^[0-9]+,[0-9]+$ ]] || fail '--pair 使用网卡数组下标，例如 0,1'
    IFS=, read -r from to <<< "$PAIR"
    (( from < ${#NIC[@]} && to < ${#NIC[@]} && from != to )) || fail '网卡数组下标无效'
    python3 "$SCRIPT_DIR/1825-path-probe.py" "$PROTO" "${NIC[$from]}" "${NIC[$to]}" "${V4HOST[$from]}" "${V4HOST[$to]}" "${V6HOST[$from]}" "${V6HOST[$to]}"
    ;;
  hccn) write_hccn ;;
  rollback) rollback ;;
esac
