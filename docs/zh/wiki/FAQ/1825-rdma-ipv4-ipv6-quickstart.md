# UBX 1825 RDMA IPv4 / IPv6 快速开始

按顺序执行本页：先在宿主机设置公共变量并下载脚本，再采集现场、导入交换机规划表、配置交换机和主机，最后在 `work-docker` 容器运行 MF。各步骤专用的参数在使用时设置。任一步报错或核对结果不符时，先排查并重试，不要继续后续配置。`work-docker` 是容器标识示例，按现场修改。

## 1. 提前准备

在**宿主机 root shell**执行，根据现场确认工作目录和容器名。下面只设置贯穿流程的公共变量；交换机账号、机器标识、地址、MTU 和测试参数在对应步骤按现场设置。

```bash
export MF_WORKSPACE=/mnt/workspaces/memfabric-1825
export MF_SCRIPTS="$MF_WORKSPACE/scripts"
export MF_OUT_DIR="$MF_WORKSPACE/runs/1825-collect-$(date +%Y%m%d-%H%M%S)"
export MF_ENV="$MF_OUT_DIR/env.sh"
export MF_PROTOCOL=both
export MF_RUN_FAMILY=ipv6
export MF_CONTAINER=work-docker
```

`MF_OUT_DIR` 是本次运行的专用目录，后续步骤共用。请在执行第 3.1 节前选择要配置的地址族和 MF 本次使用的地址族：

| 目标 | `MF_PROTOCOL` | `MF_RUN_FAMILY` |
|---|---|---|
| 只配置和测试 IPv4 | `v4` | `ipv4` |
| 只配置和测试 IPv6 | `v6` | `ipv6` |
| 配置双栈，先测试 IPv4 | `both` | `ipv4` |
| 配置双栈，先测试 IPv6 | `both` | `ipv6` |

例如只运行 IPv4，在执行第 3.1 节前执行：

```bash
export MF_PROTOCOL=v4
export MF_RUN_FAMILY=ipv4
```

后续地址、路由和抓包按 `MF_PROTOCOL` 处理；第 8 步用 `MF_RUN_FAMILY` 写入 `hccn.conf`，第 9 步直接运行所选地址族，不要求先跑另一种。后续命令应在同一个宿主机 shell 按顺序执行。

新机器需要 Python 3、`ip`、`ssh`、`lldpcli`、`npu-smi`、`tcpdump` 和 `ping`。MF 容器通过 run 包安装；run 包会安装 whl 并提供 `mfcli`。`/etc/hccn.conf` 要在宿主机和容器中共享。示例工作目录不得与其他业务共用，且必须以相同路径挂载到容器，因为第 9 步要读取生成的 `env.sh`。

将下面七个链接对应的同一版本脚本下载到 `$MF_SCRIPTS`，无需克隆仓库：

| 文件 | 用途 |
|---|---|
| [1825-auto.py](../../scripts/1825-auto.py) | 读取预填信息，自动完成交换机和主机配置。 |
| [1825-collect.py](../../scripts/1825-collect.py) | 连接交换机，采集主机和交换机快照并生成环境文件。 |
| [1825-env-probe.py](../../scripts/1825-env-probe.py) | 从拓扑、LLDP 和快照生成环境变量，由采集脚本调用。 |
| [env-probe.sh](../../scripts/env-probe.sh) | `1825-env-probe.py` 的命令入口，供手工采集路径使用。 |
| [1825-deploy.sh](../../scripts/1825-deploy.sh) | 生成交换机命令、配置和检查主机、修改 hccn 以及回滚。 |
| [1825-path-probe.py](../../scripts/1825-path-probe.py) | 检查双端实际抓包，由部署脚本的 `--probe` 调用。 |
| [1825-pfc.py](../../scripts/1825-pfc.py) | 生成并核对 DSCP 信任方式与队列 4 的 PFC 配置。 |

在宿主机创建脚本目录：

```bash
mkdir -p "$MF_SCRIPTS" "$MF_WORKSPACE/runs"
```

下载完成后确认七个文件存在：

```bash
ls -l "$MF_SCRIPTS/"{1825-auto.py,1825-collect.py,1825-env-probe.py,env-probe.sh,1825-deploy.sh,1825-path-probe.py,1825-pfc.py}
```

## 2. 自动化配置

此入口根据规划表配置交换机、主机网络和 `/etc/hccn.conf`，执行完成后停止，不运行 MF。先在宿主机按第 1 节下载 7 个脚本（下载表格和 `ls` 命令在第 1 节），再设置工作目录和脚本目录：

```bash
export MF_WORKSPACE=/mnt/workspaces/memfabric-1825
export MF_SCRIPTS="$MF_WORKSPACE/scripts"
mkdir -p "$MF_SCRIPTS" "$MF_WORKSPACE/runs"
```

第 3 节是手工流程，可用来核对和排障；自动配置成功后不需要重复执行配置命令。

先将规划表保存为宿主机上的 TSV 文件。表格必须是 `交换机口、物料编码、IPv4、IPv6、对应机器` 五列，以 Tab 分隔并包含表头；每行一个子口，IPv4/IPv6 填**网关**地址（`.1` / `::1`），同一物理口的第二个子口交换机口留空。单栈时未选地址族的列留空。格式如下（表格只是示意，文件里必须是 Tab，不是空格或竖线）：

| 交换机口 | 物料编码 | IPv4 | IPv6 | 对应机器 |
|---|---|---|---|---|
| 37 | 9 | 171.168.49.1 | 2001:C49::1 | 86-左1 |
|  | 10 | 171.168.50.1 | 2001:C50::1 | 86-左2 |
| 38 | 11 | 171.168.51.1 | 2001:C51::1 | 86-左3 |
|  | 12 | 171.168.52.1 | 2001:C52::1 | 86-左4 |
| 39 | 13 | 171.168.53.1 | 2001:C53::1 | 86-左5 |
|  | 14 | 171.168.54.1 | 2001:C54::1 | 86-左6 |
| 40 | 15 | 171.168.55.1 | 2001:C55::1 | 86-左7 |
|  | 16 | 171.168.56.1 | 2001:C56::1 | 86-左8 |

更完整的规划规则和导入步骤见第 3.3 节“1. 原样存成 TSV”。例如，将表格从 Excel 粘贴到工作目录时：

```bash
if [ -e "$MF_WORKSPACE/plan-raw.tsv" ]; then
  echo "规划表已存在，请核对：$MF_WORKSPACE/plan-raw.tsv"
else
  echo '粘贴五列 TSV 表格后，在新的一行按 Ctrl+D 保存。'
  cat > "$MF_WORKSPACE/plan-raw.tsv"
fi
```

再创建配置文件，并按现场修改交换机、规划表、机器标识和地址族。示例使用工作目录 `/mnt/workspaces/memfabric-1825`，如果第 1 节使用了其他目录，需要同时修改 JSON 的 `workspace` 和 `plan_tsv`。`switch_password` 留空时使用已经配置的 SSH 免密；填写密码时宿主机需要安装 `sshpass`。

```bash
umask 077
cat > "$MF_WORKSPACE/1825-auto.json" <<'EOF'
{
  "workspace": "/mnt/workspaces/memfabric-1825",
  "plan_tsv": "/mnt/workspaces/memfabric-1825/plan-raw.tsv",
  "machine_id": "86",
  "switch_host": "90.90.92.13",
  "switch_user": "huawei",
  "switch_password": "",
  "protocol": "v4",
  "run_family": "ipv4",
  "host_suffix": 2,
  "host_mtu": 9000,
  "vlan_mtu": 9000,
  "vlans": {},
  "qos_commands": null
}
EOF
chmod 600 "$MF_WORKSPACE/1825-auto.json"
```

`machine_id` 按规划表“对应机器”列的前缀匹配，必须只匹配本机。`host_suffix` 是主机地址尾号，不能与网关或其他设备冲突。自动推导地址时，IPv4 网关必须为 `/24` 网段的 `.1`，IPv6 网关必须为 `/64` 网段的 `::1`。`protocol` 和 `run_family` 按第 1 节的四种组合填写；单栈不需要规划另一个地址族。

`vlans` 为空时按“物理口×10+子口”生成 VLAN，例如 `37:1` 使用 `371`。如果现场规划不同，可填写 `"vlans": {"37:1": 491, "37:2": 492}`；未列出的端口仍使用默认规则。`qos_commands` 为 `null` 时沿用采集到的端口 QoS；采集不到或各口不一致时，需要根据现场网络管理员提供的配置填写命令数组。

保存文件后，在宿主机运行：

```bash
python3 "$MF_SCRIPTS/1825-auto.py" --config "$MF_WORKSPACE/1825-auto.json"
```

脚本依次采集主机和交换机、检查端口与 VLAN、通过 SSH 进入交换机执行并提交配置、配置主机 IP 和路由、检查规则，最后写入所选地址族的 `hccn.conf`。已配置且符合规划的 IP 和网关会复用。每次运行生成独立目录，包含环境文件、主机及交换机快照、交换机命令和交互日志，完成时显示目录路径。

以 8 口 IPv4 环境为例，完成后的回显节选：

```text
[OK] v4 local 和每族 56 条精确规则通过
[OK] /etc/hccn.conf: 16 个 device 已配置，地址族 ipv4
[OK] 交换机、主机网络和 hccn.conf 配置完成，记录: /mnt/workspaces/memfabric-1825/runs/1825-auto-...
```

如果交换机或主机配置与规划冲突、只有部分端口配置了地址，或者交换机命令报错，脚本会停止并输出原因。之前已提交的交换机命令不会自动撤销；请根据输出目录中的快照和日志核对后再处理。完成配置后，如需验证 MF，手工执行第 3.3 节第 9 步，并确保宿主机的 `/etc/hccn.conf` 已挂载到容器。

## 3. 手动配置

以下为手工流程，用于核对自动配置结果或排障；自动配置成功后可跳过本节。

### 3.1 采集主机和交换机

在宿主机执行。先在下面的命令中填写交换机用户名；`MF_SWITCH_IP` 留空时从 LLDP 自动获取，无法自动获取时填交换机管理地址。脚本只读取现场配置，可能提示输入交换机 SSH 密码；读取 Vlanif 时可能需要再次输入。它不会读取你提供的规划表，TSV 在第 3.3 节导入。

```bash
MF_SWITCH_USER=huawei
MF_SWITCH_IP=
switch_args=()
if [ -n "$MF_SWITCH_IP" ]; then switch_args=(--switch "$MF_SWITCH_IP"); fi
if [[ "$MF_PROTOCOL:$MF_RUN_FAMILY" != v4:ipv4 && "$MF_PROTOCOL:$MF_RUN_FAMILY" != v6:ipv6 &&
      "$MF_PROTOCOL:$MF_RUN_FAMILY" != both:ipv4 && "$MF_PROTOCOL:$MF_RUN_FAMILY" != both:ipv6 ]]; then
  echo 'MF_PROTOCOL 和 MF_RUN_FAMILY 不匹配，请按第 1 节重新选择。' >&2
  false
elif python3 "$MF_SCRIPTS/1825-collect.py" --out-dir "$MF_OUT_DIR" --switch-user "$MF_SWITCH_USER" "${switch_args[@]}"; then
  if ! grep -q '^PROTO=' "$MF_ENV"; then
    echo '采集结果缺少 PROTO，请检查下载的脚本版本。' >&2
    false
  elif sed -i "s/^PROTO=.*/PROTO=$MF_PROTOCOL/" "$MF_ENV" &&
    declare -p MF_WORKSPACE MF_SCRIPTS MF_OUT_DIR MF_ENV MF_PROTOCOL MF_RUN_FAMILY MF_CONTAINER \
      > "$MF_OUT_DIR/session-vars.sh" && chmod 600 "$MF_OUT_DIR/session-vars.sh"; then
    ls -l "$MF_ENV" "$MF_OUT_DIR/host-address.json" "$MF_OUT_DIR/switch-snapshot.txt" "$MF_OUT_DIR/session-vars.sh"
  else
    echo '保存环境变量失败，请检查输出目录和权限。' >&2
    false
  fi
else
  echo '采集失败，请检查报错；重新执行第 1 节以选择新的运行目录。' >&2
  false
fi
```

LLDP 找不到端口、对应多个交换机或系统名不匹配时脚本会报错，不能按网卡排序猜测连接关系。若提示某 HCA 没有网卡，或网卡没有 LLDP 邻居，先在宿主机使用 `ls /sys/class/infiniband/*/device/net` 和 `lldpcli show neighbors` 检查设备、网络命名空间和链路；恢复后重新采集，不要继续部署。采集脚本生成的默认 `PROTO` 会自动与第 1 节的 `MF_PROTOCOL` 对齐，后续测试地址族由 `MF_RUN_FAMILY` 指定。`session-vars.sh` 保存第 1 节的公共变量，不含交换机密码；重新登录时，按实际路径执行 `source /实际运行目录/session-vars.sh` 恢复公共变量，再执行 `source "$MF_ENV"` 加载采集数组。从中间步骤继续时，还需重新设置该步骤使用的参数。

实测回显示例（86 清理后的现场，端口名为实际输出）：

```text
LLDP 交换机: Z3-1-B24-GS0-2，管理地址: 90.90.92.13，端口: 800GE1/0/37:1, 800GE1/0/37:2, 800GE1/0/38:1, 800GE1/0/38:2, 800GE1/0/39:1, 800GE1/0/39:2, 800GE1/0/40:1, 800GE1/0/40:2
将通过 SSH 执行只读 display 命令；可能需要输入交换机密码。
huawei@90.90.92.13's password:
[WARN] 端口没有非默认 PVID；VLAN 和网关需人工规划。
已生成 /mnt/workspaces/memfabric-1825/runs/1825-collect-20261113-192529/env.sh；未修改网络配置。
网卡、交换机端口、device 来源：sysfs、LLDP、NPU topo；空白项需人工确认。
VLAN、网关、QoS 来源：交换机快照；无快照时需人工规划。
  ens100f0 | 800GE1/0/37:1 | 0,1 | 待填写 | 待填写 | 待填写 | 100
  ens101f0 | 800GE1/0/37:2 | 2,3 | 待填写 | 待填写 | 待填写 | 101
  ens102f0 | 800GE1/0/38:1 | 4,5 | 待填写 | 待填写 | 待填写 | 102
  ens103f0 | 800GE1/0/38:2 | 6,7 | 待填写 | 待填写 | 待填写 | 103
  enP1s104f0 | 800GE1/0/39:1 | 8,9 | 待填写 | 待填写 | 待填写 | 104
  enP1s105f0 | 800GE1/0/39:2 | 10,11 | 待填写 | 待填写 | 待填写 | 105
  enP1s106f0 | 800GE1/0/40:1 | 12,13 | 待填写 | 待填写 | 待填写 | 106
  enP1s107f0 | 800GE1/0/40:2 | 14,15 | 待填写 | 待填写 | 待填写 | 107
核对所有映射、VLAN/IP 规划、表号和管理口后，将 PLAN_CONFIRMED 改为 1。
主机快照: /mnt/workspaces/memfabric-1825/runs/1825-collect-20261113-192529/host-address.json
交换机快照: /mnt/workspaces/memfabric-1825/runs/1825-collect-20261113-192529/switch-snapshot.txt
环境文件: /mnt/workspaces/memfabric-1825/runs/1825-collect-20261113-192529/env.sh
```

同一命令在两种现场下只差 VLAN / 网关两列，其余行相同：

| 现场 | `VLAN` / `IPv4 网关` / `IPv6 网关` |
|---|---|
| 交换机已清空（上面这份） | `待填写` |
| 交换机已有 VLAN/Vlanif | 实际值，例如 `371` / `171.168.49.1` / `2001:C49::1` |

`password:` 那行是 SSH 交互提示；目录名、inode 和时间随现场变化。

每行依次表示 `网卡 | 交换机口 | 物理 device | VLAN | IPv4 网关 | IPv6 网关 | 路由表号`。例如上面一行表示 `ens100f0` 连接 `800GE1/0/37:1`，对应 device `0,1` 和路由表 `100`。`待填写` 表示当前采集不到，不能据此判断网络已配置完成。

运行以下命令查看生成的环境变量：

```bash
cat "$MF_ENV"
```

实测片段及含义：

```bash
PLAN_CONFIRMED=0
MGT_IF=virbr0
NIC=(ens100f0 ens101f0 ...)  # 物理数据面网卡，来自 HCA sysfs
SPORT=(800GE1/0/37:1 800GE1/0/37:2 ...)  # LLDP 发现的交换机口
DEV=(0,1 2,3 ...)  # NPU 拓扑和物理 device 编号
SVLAN=('' '' ...)  # 端口 VLAN；空值需要规划
V4HOST=('' '' ...)  # 主机 IPv4；空值需要规划
V6HOST=('' '' ...)  # 主机 IPv6；空值需要规划
TABLE=(100 101 ...)  # 未被本机路由和规则占用的候选表号
HOST_MTU=(9000 9000 ...)  # 采集时的主机网卡 MTU
```

上面的省略号只表示省略的数组项，不要复制成配置。各数组按 `NIC` 的顺序对应同一张网卡：

| 变量 | 含义和来源 |
|---|---|
| `MGT_IF` | 当前 IPv4 默认路由对应的管理接口；部署时不得作为数据面接口。 |
| `NIC`、`SPORT`、`DEV` | 本机网卡、LLDP 交换机口、NPU 物理 device 对应关系。 |
| `SVLAN`、`SV4GW`、`SV6GW`、`VLAN_MTU` | 交换机端口 VLAN 和 Vlanif 网关、MTU；来自交换机当前配置。 |
| `V4NET`、`V4HOST`、`V4LEN` | 主机 IPv4 网段、地址和前缀长度；来自主机当前地址。 |
| `V6PFX`、`V6HOST`、`V6LEN` | 主机 IPv6 前缀、地址和前缀长度；来自主机当前地址。 |
| `TABLE` | 排除本机已有路由、规则和注册表后得到的候选路由表号；仍须核对预留。 |
| `HOST_MTU` | 采集时网卡的 MTU，需要核对是否是目标值。 |
| `SW_QOS_CMDS`、`TRANSCEIVER` | 交换机端口 QoS/PFC 和光模块类型；来自交换机当前配置。 |
| `PROTO`、`PLAN_CONFIRMED` | 与第 1 节同步的协议族和人工核对状态。 |

已有地址和配置时可以采到这些值；新环境未配置或已清空时，地址、VLAN 和网关需要人工规划。全部核对后，再按后续步骤设置 `PLAN_CONFIRMED=1`。

如果 LLDP 没有提供唯一的交换机管理 IPv4，在本节采集命令中填写 `MF_SWITCH_IP` 后重新采集。脚本会核对交换机系统名与 LLDP；发现多台交换机、端口缺失、交换机地址不符或文件已存在时会报错，不会猜测。失败后若已有快照文件，选择新的 `MF_OUT_DIR` 重试。

3.1 节生成的 `env.sh` 保存了自动采集的网卡、端口、device 和 QoS；它没有读取你的 TSV。`source "$MF_ENV"` 会加载这些自动生成的数组。后续规划转换只补充已核对的 VLAN、地址和 MTU，手工变量仍以第 1 节为准。所有命令在同一宿主机 shell 中按顺序执行；交换机命令复制到交换机会话。

### 3.2 规划规则

交换机没有任何数据面配置（端口还在 VLAN 1、没有 Vlanif）时，上面这些值采集不到，必须先规划；每口一行，规划完填进 `env.sh`。

| 项 | 取值规则 |
|---|---|
| VLAN | **自定义**：交换机 `display vlan` 里挑未被使用、成员为空的 VLAN，每口一个，只需互不相同 |
| IPv4 网段 / 网关 | 每口独立 `x.x.x.0/24`，网关取 `.1` |
| IPv6 前缀 / 网关 | 每口独立 `/64`，网关取 `::1` |
| 主机地址 | **自定义**：网关同网段的 `.2`；同一网段接第二台机器用 `.3`。唯一、不与网关和现网设备冲突即可 |
| 路由表号 | `ip -4 route show table N` 和 `ip -6 route show table N` 都为空，且 `ip rule show` 无引用 |
| MTU | 在生成环境文件时设置主机网卡和 Vlanif 的 MTU，示例为 9000；本系列二层物理子口不支持 `mtu` 命令 |
| QoS / PFC | 用芯片厂商推荐值，不要照抄其他现场的 `33280` |
| 光模块 | `display interface <端口> transceiver verbose` 查实际类型 |

VLAN 和主机地址都是**自定义值**，判据只有两条：不冲突、两端一致。

- VLAN 只存在于交换机侧（端口 PVID + Vlanif），主机不感知；同一台交换机上每口一个、未被其他业务占用即可。
- 主机地址只存在于主机侧，取网关同网段；`.2`/`.3` 只是约定，不是硬性规则，但同一网段内不能重复。
- 管理口（如 `virbr0`）不在数据面网段内，不参与上面的规划。
- 两台机器互连时（如 86↔87），VLAN、网段、网关必须一致，只有主机地址不同（`.2` 与 `.3`）。

规划前要确认 VLAN 没有被其他交换机或业务占用，脚本无法证明这一点。下面的地址转换适用于 IPv4 `/24`、IPv6 `/64`，且网关分别以 `.1` 和 `::1` 结尾的规划；其他规划需调整转换逻辑并核对生成结果。单栈时只填写所选地址族的规划，另一地址族保留空列。规划完成后，继续下面的九步。

### 3.3 按设备规划表执行九步

**前提**：先执行 3.1 节自动采集，生成 `$MF_ENV`。即使 VLAN 和地址已清空，脚本仍会采集端口现有 QoS；86 的 8 口 QoS 一致；命令条数取决于现场配置。若采集结果提示 `SW_QOS_CMDS` 为空或各口不一致，必须先按现场确认的 QoS/PFC 规划填写，不能用空数组继续。网络环境没有已配置 QoS 时，需要由网络管理员提供；脚本不会自动生成厂商参数。

确认采集结果，以下命令在宿主机执行：

```bash
source "$MF_ENV"
printf '网卡数=%s QoS命令数=%s\n' "${#NIC[@]}" "${#SW_QOS_CMDS[@]}"
```

86 实测回显：

```text
网卡数=8 QoS命令数=25
```

很多现场只给一张表：`交换机口 | 物料编码 | IPv4 | IPv6 | 对应机器`，两个子口并排或上下排列，**没有 VLAN**。按下面 9 步导入并核对规划，再进行 MF 验证。

每一步的产物/效果：

| 步骤 | 做什么 | 产物或效果 | 在哪执行 |
|---|---|---|---|
| 1 | 导入规划表 | `$MF_PLAN_RAW` 指向 TSV 文件（5 列 + 表头） | 主机 |
| 2 | 筛出本机各口并补 VLAN | `$MF_OUT_DIR/plan.tsv`（4 列） | 主机 |
| 3 | 推地址并写入 env | `$MF_ENV` 末尾多 12 行；末尾 `--check` 打印映射表 | 主机 |
| 4 | 生成交换机基础命令 | `$MF_OUT_DIR/switch-cmds.txt`，不含 IP 地址 | 主机 |
| 5 | 执行并核对交换机 | 核对 VLAN、端口 PVID 和端口桥 | 交换机 |
| 5a | 配置 IPv4 和 IPv6 地址 | 所选地址族的交换机网关和主机地址；已符合规划时跳过配置 | 交换机、主机 |
| 6 | 配置主机路由和规则 | 检查地址，配置或复用路由及 8×7 条精确规则 | 主机 |
| 7 | 断言 + 抓包 | `--verify` 通过；`--probe` 双口抓到 ICMP | 主机 |
| 8 | 写 hccn.conf | 16 行 `address_N`，inode 不变 | 主机 |
| 9 | 跑 MF | 八方向带宽表，退出 0 | 容器 |

按下列 1–9 步执行一次。自动采集没有读取 TSV；第 1–3 步会把规划表中的网关写入环境文件。所选地址族已配置时可以跳过第 5a 步的配置命令，仍需执行后续路由、抓包、hccn 和 MF 步骤。

**1. 原样存成 TSV**：Excel 里选中区域复制，粘到 `MF_PLAN_RAW` 指向的文件。要求：

- Tab 分隔，保留表头；**不要**按下面的 Markdown 表格格式存，表格只是示意
- 一行对应一个子口，所选地址族填写**网关**地址（IPv4 的 `.1` 或 IPv6 的 `::1`）；单栈时未选地址族保留空列，仍用 Tab 分隔五列

下表展示用户提供的 86 对照；下面的示例文件还包括原表左半部分的 81 数据。原表右半部分没有明确对应机器，不自动纳入，使用时需补充机器归属：

| 交换机口 | 物料编码 | IPv4 | IPv6 | 对应机器 |
|---|---|---|---|---|
| 37 | 9 | 171.168.49.1 | 2001:C49::1 | 86-左1 |
|  | 10 | 171.168.50.1 | 2001:C50::1 | 86-左2 |
| 38 | 11 | 171.168.51.1 | 2001:C51::1 | 86-左3 |
|  | 12 | 171.168.52.1 | 2001:C52::1 | 86-左4 |
| 39 | 13 | 171.168.53.1 | 2001:C53::1 | 86-左5 |
|  | 14 | 171.168.54.1 | 2001:C54::1 | 86-左6 |
| 40 | 15 | 171.168.55.1 | 2001:C55::1 | 86-左7 |
|  | 16 | 171.168.56.1 | 2001:C56::1 | 86-左8 |

自动采集不会读取这张表。新环境必须提供本机的实际规划，不能使用 86 的示例地址。TSV 只保留上面五列；原表有左右两组时，先将本机所在的一组整理成这五列。需要每个子口各有一行，合并的交换机口可以留空，但同一物理口的两行必须相邻。

如果规划表已经是宿主机上的 TSV 文件，将下面的路径改成实际绝对路径并执行：

```bash
MF_PLAN_RAW=/实际路径/plan-raw.tsv
```

如果需要从 Excel 粘贴，在宿主机执行以下命令。提示出现后，粘贴包含表头的五列内容，再单独按 `Ctrl+D` 保存；列之间必须是 Tab。已有同名文件时，命令会保留文件，不进入粘贴：

```bash
MF_PLAN_RAW="$MF_OUT_DIR/plan-raw.tsv"
if [ -e "$MF_PLAN_RAW" ]; then
  echo "规划表已存在，请核对：$MF_PLAN_RAW"
else
  echo '粘贴 TSV 表格后，在新的一行按 Ctrl+D 保存。'
  cat > "$MF_PLAN_RAW"
fi
```

无论采用哪种方式，都先检查文件路径、列数和内容，再执行第 2 步：

```bash
if [ ! -s "$MF_PLAN_RAW" ]; then
  echo "规划表不存在或为空：$MF_PLAN_RAW" >&2
  false
else
  awk -F '\t' 'NF != 5 { printf "第 %d 行有 %d 列，需要 5 列\n", NR, NF; bad=1 } END { exit bad }' "$MF_PLAN_RAW" && \
    head -5 "$MF_PLAN_RAW"
fi
```

回显示例（列之间是真正的 Tab；实际地址和机器标识以现场规划为准）：

```text
交换机口  物料编码  IPv4          IPv6          对应机器
37        9         171.168.49.1  2001:C49::1  86-左1
          10        171.168.50.1  2001:C50::1  86-左2
38        11        171.168.51.1  2001:C51::1  86-左3
          12        171.168.52.1  2001:C52::1  86-左4
```

若只规划 IPv4，IPv6 列留空；只规划 IPv6 时，IPv4 列留空。空列也要用 Tab 保留，不能把表格另存为 XLSX 或逗号分隔的 CSV 后直接用于下面的命令。物料编码只作对照，device 编号来自第 3.1 节的拓扑采集。

**2. 转成四列 `plan.tsv`**（按 `MF_MACHINE_ID` 筛选，VLAN 按“端口×10+子口”生成候选值：37:1→371、37:2→372；必须先确认候选 VLAN 未被其他业务使用，不合适时修改输出文件）。执行前将下方 `MF_MACHINE_ID` 改成规划表“对应机器”列的本机前缀。脚本按前缀匹配，例如 `86` 匹配 `86-左1`；请检查该前缀不会匹配其他机器。子口编号按原表中同一物理口的全部行计算，再筛选本机：

```bash
MF_MACHINE_ID=86
MF_PLAN="$MF_OUT_DIR/plan.tsv"
if [ -e "$MF_PLAN" ]; then
  echo "规划文件已存在，请检查或更换 MF_PLAN：$MF_PLAN"
else
awk -F'\t' -v m="$MF_MACHINE_ID" 'BEGIN{OFS="\t"}
  NR==1 { next }
  { if ($1 != "") p = $1
    if (p != "") s[p]++
    if ($5 ~ ("^" m)) { printf "%s:%d\t%d\t%s\t%s\n", p, s[p], p*10+s[p], $3, $4 } }
' "$MF_PLAN_RAW" > "$MF_PLAN"
fi
cat "$MF_PLAN"
```

按采集到的网卡数核对行数。86 双栈示例为 8 行（实际文件用 Tab 分隔，下面用空格对齐显示）：

```text
37:1    371    171.168.49.1    2001:C49::1
37:2    372    171.168.50.1    2001:C50::1
38:1    381    171.168.51.1    2001:C51::1
38:2    382    171.168.52.1    2001:C52::1
39:1    391    171.168.53.1    2001:C53::1
39:2    392    171.168.54.1    2001:C54::1
40:1    401    171.168.55.1    2001:C55::1
40:2    402    171.168.56.1    2001:C56::1
```

行数应与采集到的网卡数一致；不一致时检查“对应机器”列是否匹配 `MF_MACHINE_ID`、端口列是否被合并成一行。86 的示例若使用 `491/492`，可将第 5、6 行的 `391/392` 改为对应值；所有 VLAN 和地址仍须先核对是否可用。

规划表的行顺序必须与采集到的 `SPORT` 数组一致；第 3 步按行顺序写数组。继续前对照下列输出，两列必须逐行相同：

```bash
paste <(printf '%s\n' "${SPORT[@]##*/}") <(cut -f1 "$MF_PLAN")
```

回显示例：

```text
37:1    37:1
37:2    37:2
...
40:2    40:2
```

**3. 生成并写入 `env.sh`**。执行前按现场填写本步的主机地址尾号和 MTU：

这一步只改文件、不动网络，具体做四件事：

1. 逐行读 `$MF_PLAN`，按行里的 `VLAN / v4网关 / v6网关` 生成 地址、VLAN 和 MTU 数组；
2. 从所选地址族的网关反推其余地址：`171.168.49.1` → 网段 `171.168.49.0`、主机 `171.168.49.<MF_HOST_SUFFIX>`（示例为 2，第二台机器可用 3）；`2001:C49::1` → 前缀 `2001:c49::`、主机 `2001:c49::2`；
3. 固定 `V4LEN=24`、`V6LEN=64`，主机和 Vlanif MTU 使用本步设置的值；
4. 把这 12 行追加到 `$MF_ENV` 末尾，最后一行 `PLAN_CONFIRMED=1`。

追加而不是替换，是因为 `env.sh` 前面采集阶段已经写了同名的空数组；bash 后赋值生效，末尾这份覆盖前面的空值，因此不用手工改文件。重复执行会把同样 12 行再追加一遍（值相同、结果不变，只是文件变长），要重跑就换一个新的 `$MF_ENV`。

```bash
MF_HOST_SUFFIX=2
MF_HOST_MTU=9000
MF_VLAN_MTU=9000
if diff -u <(printf '%s\n' "${SPORT[@]##*/}") <(cut -f1 "$MF_PLAN"); then
awk -F'\t' -v ho="$MF_HOST_SUFFIX" -v host_mtu="$MF_HOST_MTU" -v vlan_mtu="$MF_VLAN_MTU" '
  function value(x) { return x == "" ? "\"\"" : x }
  /^#/ || NF < 3 { next }
  { n++; vlan[n]=$2; g4[n]=$3; g6[n]=tolower($4)
    if (g4[n] != "") { split(g4[n],a,"."); net[n]=a[1]"."a[2]"."a[3]".0"; h4[n]=a[1]"."a[2]"."a[3]"."ho }
    if (g6[n] != "") { split(g6[n],b,"::"); pfx[n]=b[1]"::"; h6[n]=b[1]"::"ho } }
  END { printf "SVLAN=(";    for(i=1;i<=n;i++) printf "%s%s",vlan[i],(i<n?" ":""); print ")"
        printf "SV4GW=(";    for(i=1;i<=n;i++) printf "%s%s",value(g4[i]),(i<n?" ":"");   print ")"
        printf "SV6GW=(";    for(i=1;i<=n;i++) printf "%s%s",value(g6[i]),(i<n?" ":"");   print ")"
        printf "V4NET=(";    for(i=1;i<=n;i++) printf "%s%s",value(net[i]),(i<n?" ":"");  print ")"
        printf "V4HOST=(";   for(i=1;i<=n;i++) printf "%s%s",value(h4[i]),(i<n?" ":"");   print ")"
        printf "V4LEN=(";    for(i=1;i<=n;i++) printf "24%s",(i<n?" ":"");         print ")"
        printf "V6PFX=(";    for(i=1;i<=n;i++) printf "%s%s",value(pfx[i]),(i<n?" ":"");  print ")"
        printf "V6HOST=(";   for(i=1;i<=n;i++) printf "%s%s",value(h6[i]),(i<n?" ":"");   print ")"
        printf "V6LEN=(";    for(i=1;i<=n;i++) printf "64%s",(i<n?" ":"");         print ")"
        printf "HOST_MTU=("; for(i=1;i<=n;i++) printf "%s%s",host_mtu,(i<n?" ":""); print ")"
        printf "VLAN_MTU=("; for(i=1;i<=n;i++) printf "%s%s",vlan_mtu,(i<n?" ":""); print ")"
        print "PLAN_CONFIRMED=1" }' "$MF_PLAN" >> "$MF_ENV"

source "$MF_ENV" && echo "口数=${#SVLAN[@]} VLAN=${SVLAN[*]} HOST=${V4HOST[*]}"
bash "$MF_SCRIPTS/1825-deploy.sh" --env "$MF_ENV" --protocol "$MF_PROTOCOL" --check
else
  echo '规划表端口顺序或数量与 SPORT 不一致，未写入 env.sh。' >&2
  false
fi
```

`$MF_ENV` 末尾多出的就是这 12 行（86 的双栈值）；单栈时未选地址族的地址数组为空值：

```bash
SVLAN=(371 372 381 382 391 392 401 402)
SV4GW=(171.168.49.1 171.168.50.1 171.168.51.1 171.168.52.1 171.168.53.1 171.168.54.1 171.168.55.1 171.168.56.1)
SV6GW=(2001:c49::1 2001:c50::1 2001:c51::1 2001:c52::1 2001:c53::1 2001:c54::1 2001:c55::1 2001:c56::1)
V4NET=(171.168.49.0 171.168.50.0 171.168.51.0 171.168.52.0 171.168.53.0 171.168.54.0 171.168.55.0 171.168.56.0)
V4HOST=(171.168.49.2 171.168.50.2 171.168.51.2 171.168.52.2 171.168.53.2 171.168.54.2 171.168.55.2 171.168.56.2)
V4LEN=(24 24 24 24 24 24 24 24)
V6PFX=(2001:c49:: 2001:c50:: 2001:c51:: 2001:c52:: 2001:c53:: 2001:c54:: 2001:c55:: 2001:c56::)
V6HOST=(2001:c49::2 2001:c50::2 2001:c51::2 2001:c52::2 2001:c53::2 2001:c54::2 2001:c55::2 2001:c56::2)
V6LEN=(64 64 64 64 64 64 64 64)
HOST_MTU=(9000 9000 9000 9000 9000 9000 9000 9000)
VLAN_MTU=(9000 9000 9000 9000 9000 9000 9000 9000)
PLAN_CONFIRMED=1
```

`source` 那行在 86 双栈或 IPv4 环境中的回显如下；IPv6-only 时 `HOST` 中的 IPv4 地址为空，需查看上面的 `V6HOST` 数组：

```text
口数=8 VLAN=371 372 381 382 391 392 401 402 HOST=171.168.49.2 171.168.50.2 171.168.51.2 171.168.52.2 171.168.53.2 171.168.54.2 171.168.55.2 171.168.56.2
```

接着 `--check` 通过会打印映射表并退出 0：

```text
网卡 | 交换机口 | device | VLAN | IPv4 网关 | IPv6 网关 | 表
ens100f0 | 800GE1/0/37:1 | 0,1 | 371 | 171.168.49.1 | 2001:c49::1 | 100
ens101f0 | 800GE1/0/37:2 | 2,3 | 372 | 171.168.50.1 | 2001:c50::1 | 101
ens102f0 | 800GE1/0/38:1 | 4,5 | 381 | 171.168.51.1 | 2001:c51::1 | 102
ens103f0 | 800GE1/0/38:2 | 6,7 | 382 | 171.168.52.1 | 2001:c52::1 | 103
enP1s104f0 | 800GE1/0/39:1 | 8,9 | 391 | 171.168.53.1 | 2001:c53::1 | 104
enP1s105f0 | 800GE1/0/39:2 | 10,11 | 392 | 171.168.54.1 | 2001:c54::1 | 105
enP1s106f0 | 800GE1/0/40:1 | 12,13 | 401 | 171.168.55.1 | 2001:c55::1 | 106
enP1s107f0 | 800GE1/0/40:2 | 14,15 | 402 | 171.168.56.1 | 2001:c56::1 | 107
```

报 `VLAN 无效`、`缺少数组`、`管理接口不匹配` 时按提示改 `plan.tsv` 或 `env.sh`，不要直接进第 4 步。

本流程每次调用部署脚本都使用第 1 节的 `MF_PROTOCOL`；`v4`、`v6` 和 `both` 分别配置 IPv4、IPv6 和双栈，采集后的 `env.sh` 与该值一致。

注意：追加在 `env.sh` 末尾，后赋值覆盖前面的空值，`PLAN_CONFIRMED=1` 也一并写好了；不要再用编辑器手工改那一行。表里 39 口若写 `C53/C54`、而现网按 `C391/C392` 部署，改 `plan.tsv` 那两行前缀即可，但交换机、`hccn.conf`、`env.sh` 三处必须一致。

**4. 生成交换机基础命令**（只生成文件，不修改交换机；所选地址族的地址在第 5a 步处理）

```bash
if [ -z "${MF_OUT_DIR:-}" ] || [ ! -d "$MF_OUT_DIR" ]; then
  echo 'MF_OUT_DIR 未设置或目录不存在，请按第 1 节设置或恢复变量。' >&2
  false
else
  bash "$MF_SCRIPTS/1825-deploy.sh" --env "$MF_ENV" --protocol "$MF_PROTOCOL" --switch --skip-ip > "$MF_OUT_DIR/switch-cmds.txt" && \
    wc -l "$MF_OUT_DIR/switch-cmds.txt" && head -5 "$MF_OUT_DIR/switch-cmds.txt"
fi
```

生成文件的行数随现场 QoS 配置变化。下面是双栈示例的部分内容；单 IPv4 配置不会生成 `ipv6 enable`，第 5a 步再配置所选网关地址：

```text
system-view
vlan batch 371
commit
interface 800GE1/0/37:1
portswitch
...
interface Vlanif 371
mtu 9000
ipv6 enable
commit
quit
...
return
```

**5. 在交换机上执行并核对基础配置**

把 `switch-cmds.txt` 的全部内容粘进交换机会话（从 `system-view` 到 `return`）。在宿主机生成核对命令：

```bash
if [ -z "${MF_OUT_DIR:-}" ] || [ ! -d "$MF_OUT_DIR" ]; then
  echo 'MF_OUT_DIR 未设置或目录不存在，请按第 1 节设置或恢复变量。' >&2
  false
else
  {
    for v in "${SVLAN[@]}"; do echo "display vlan $v"; done
    for p in "${SPORT[@]}"; do echo "display current-configuration interface $p | include \"default vlan|bridge\""; done
  } > "$MF_OUT_DIR/switch-verify.txt" && cat "$MF_OUT_DIR/switch-verify.txt"
fi
```

把 `$MF_OUT_DIR/switch-verify.txt` 粘进交换机。每个 VLAN 应只包含规划的端口，端口回显应包含对应 PVID 和 `port bridge enable`。86 的 371、37:1 回显节选：

```text
<Z3-1-B24-GS0-2>display vlan 371
VID          Ports
--------------------------------------------------------------------------------
 371         UT:800GE1/0/37:1(U)

<Z3-1-B24-GS0-2>display current-configuration interface 800GE1/0/37:1 | include "default vlan|bridge"
 port default vlan 371
 port bridge enable
```

**5a. 配置所选的 IPv4 / IPv6 地址（已配置可跳过）**

此小节按第 1 节的 `MF_PROTOCOL` 处理交换机 Vlanif 网关和主机数据面地址：选 `v4` 只配置 IPv4，选 `v6` 只配置 IPv6，选 `both` 配置双栈。`MF_RUN_FAMILY` 已指定 MF 本次使用的地址族；第 6 步准备路由和状态后，第 8 步才写入 `/etc/hccn.conf`。先在宿主机核对规划与当前地址：

```bash
source "$MF_ENV"
for i in "${!NIC[@]}"; do
  printf '%s 规划 IPv4=%s/%s IPv6=%s/%s\n' "${NIC[$i]}" "${V4HOST[$i]}" "${V4LEN[$i]}" "${V6HOST[$i]}" "${V6LEN[$i]}"
  ip -brief address show dev "${NIC[$i]}"
done
```

86 已配置时的第一口回显节选：

```text
ens100f0 规划 IPv4=171.168.49.2/24 IPv6=2001:c49::2/64
ens100f0         UP             171.168.49.2/24 2001:c49::2/64 fe80::6466:66ff:fe45:eab1/64
```

在宿主机生成查询命令，复制到交换机会话检查每个 Vlanif 的网关：

```bash
for vlan in "${SVLAN[@]}"; do echo "display current-configuration interface Vlanif $vlan"; done
```

86 双栈环境第一口的网关回显节选；单栈环境只需核对所选地址族：

```text
<Z3-1-B24-GS0-2>display current-configuration interface Vlanif 371
interface Vlanif371
 mtu 9000
 ipv6 enable
 ip address 171.168.49.1 255.255.255.0
 ipv6 address 2001:C49::1/64
```

**如果两侧所选地址族的地址、网关和前缀均与规划一致，跳过本小节下面的配置命令，直接执行第 6 步。** 第 6 步会检查地址并处理路由，不依赖本小节创建的状态文件。

存在缺失地址时，先按第 1 节选择的地址族设置本小节参数：

```bash
MF_IP_PROTOCOL="$MF_PROTOCOL"
```

如果 `MF_PROTOCOL=both` 且只有一种地址族尚未配置，可把它改为 `v4` 或 `v6`，只补该地址族；例如 IPv4 已完整配置而 IPv6 尚未配置时改为 `v6`。分别核对交换机和主机：某一侧的对应地址族已完整且与规划一致时，跳过该侧的配置命令。如果同一地址族只配置了部分端口，先核对并补齐，或在确认归属后清理本方案地址；不要覆盖未知地址。

交换机的对应网关未配置时，在宿主机生成命令，然后复制完整输出到交换机会话：

```bash
bash "$MF_SCRIPTS/1825-deploy.sh" --env "$MF_ENV" --protocol "$MF_IP_PROTOCOL" --switch-ip > "$MF_OUT_DIR/switch-ip-cmds.txt" && \
  cat "$MF_OUT_DIR/switch-ip-cmds.txt"
```

86 双栈环境第一口生成命令节选；选择 `v4` 时不生成 `ipv6 address` 命令：

```text
system-view
interface Vlanif 371
ip address 171.168.49.1 255.255.255.0
ipv6 address 2001:c49::1/64
commit
quit
```

主机的对应地址族未配置时，在宿主机执行：

```bash
bash "$MF_SCRIPTS/1825-deploy.sh" --env "$MF_ENV" --protocol "$MF_IP_PROTOCOL" --ip --yes
```

双栈配置成功的回显示例：

```text
[OK] both 规划 IP 地址已配置
```

只配置 IPv4 时显示 `[OK] v4 规划 IP 地址已配置`，只配置 IPv6 时显示 `v6`。

无论是新配置还是跳过配置，都要确认每个 Vlanif 的所选网关与 `SV4GW` 或 `SV6GW` 一致。配置 IPv4 时在交换机执行：

```text
display ip interface brief | include Vlanif
```

86 的 IPv4 回显节选：

```text
Interface                   IP Address/Mask    Physical Protocol VPN
Vlanif371                   171.168.49.1/24    up       up       --
Vlanif372                   171.168.50.1/24    up       up       --
```

配置 IPv6 时执行：

```text
display ipv6 interface brief | include Vlanif
```

双栈时两条都检查。

刚配置时链路可能还在收敛；若为 `up down`，等待后重查。86 新建网关后曾出现 Vlanif 已提交但跨口抓包暂时失败；本次新建网关时，确认 `up up` 后在宿主机等待 20 秒再继续：

```bash
sleep 20
```

等待命令没有回显。已有地址并跳过本小节时不需要执行；地址或网关不一致时不要继续。

**6. 配置主机路由和规则**

```bash
bash "$MF_SCRIPTS/1825-deploy.sh" --env "$MF_ENV" --protocol "$MF_PROTOCOL" --host --skip-ip --yes
```

86 双栈新建路由时的回显；单栈时 `both` 显示为 `v4` 或 `v6`：

```text
[OK] both local 和每族 56 条精确规则通过
[OK] 已核对现有地址，并配置路由和精确规则
```

若地址和路由均已完整配置并与规划一致，脚本复用现有路由及表号：

```text
[OK] both local 和每族 56 条精确规则通过
[OK] 已复用现有 IP、路由和规则；未修改网络
```

复用的表号保存在状态目录，后续验证、抓包和 hccn 步骤自动使用；不会改写 `env.sh` 中原有的 `TABLE`。部分冲突的规则不会自动覆盖，需先核对。回滚时，主机原有的地址和复用的路由、规则会保留；本次新建的主机地址会按状态文件清理。

**7. 断言 + 双口抓包**

```bash
bash "$MF_SCRIPTS/1825-deploy.sh" --env "$MF_ENV" --protocol "$MF_PROTOCOL" --verify && \
  bash "$MF_SCRIPTS/1825-deploy.sh" --env "$MF_ENV" --protocol "$MF_PROTOCOL" --probe --pair 0,1
```

86 双栈实测回显，抓包目录每次不同；单栈时只会检查和抓取所选地址族：

```text
[OK] both local 和每族 56 条精确规则通过
2001:c50::2 from 2001:c49::2 via 2001:c49::1 dev ens100f0 table 100 metric 1024 iif lo pref medium
[OK] 2001:c49::2 -> 2001:c50::2: ens100f0 发出、ens101f0 收到各 3 个匹配请求
[OK] 2001:c50::2 -> 2001:c49::2: ens101f0 发出、ens100f0 收到各 3 个匹配请求
[OK] 171.168.49.2 -> 171.168.50.2: ens100f0 发出、ens101f0 收到各 3 个匹配请求
[OK] 171.168.50.2 -> 171.168.49.2: ens101f0 发出、ens100f0 收到各 3 个匹配请求
```

86 选择 `MF_PROTOCOL=v4` 的回显节选：

```text
[OK] v4 local 和每族 56 条精确规则通过
[OK] IPv4 规则已检查；请用 --probe 验证真实出入网卡
[OK] 171.168.49.2 -> 171.168.50.2: ens100f0 发出、ens101f0 收到各 3 个匹配请求
[OK] 171.168.50.2 -> 171.168.49.2: ens101f0 发出、ens100f0 收到各 3 个匹配请求
[OK] v4 双向 ICMP 均经过两端网卡；RoCE 硬件卸载仍需结合交换机计数验证
```

`--probe` 的 `0,1` 是 `NIC` 数组下标（第 1、2 张网卡），可换成 `0,5`、`6,7` 等；填 device 号会报“网卡数组下标无效”。

**这 56 条规则是什么**

`--host` 会为每对网卡写一条：

```text
0: from <本端地址 Vi> to <本机另一口的地址 Vj> iif lo lookup <Vi 所在口的表>
```

- 8 张网卡两两配对，N×(N−1) = 8×7 = **56**；每个已配置地址族各有 56 条规则。
- 作用范围只有“本机进程用 Vi 做源、访问本机另一个地址 Vj”。没有这条规则时，内核 `local` 表（原来 priority 0）会直接本地投递，报文不出网卡，表现就是 ping 通、网卡零流量，到 RDMA 建链阶段变成 `ret 19`。
- 规则里带 `iif lo`：只匹配本机发起（输入接口是 lo）的报文，不影响从网卡转发的流量。
- 查表指向 Vi 自己的表，表里有 `default via <该口网关>`，所以报文真正从 Vi 对应的网卡出去、经交换机到目标口的网段；同口两个 device 才需要交换机端口桥回注。

自查：

```bash
if [ "$MF_PROTOCOL" != v6 ]; then
  ip -4 rule show | grep -c 'iif lo'
fi
if [ "$MF_PROTOCOL" != v4 ]; then
  ip -6 rule show | grep -c 'iif lo'
  ip -6 route get "${V6HOST[1]}" from "${V6HOST[0]}" iif lo
fi
```

IPv6 的 `route get` 若显示 `local ... dev lo`，说明 `priority 0 lookup local` 还在，规则没生效。IPv4 使用上面的 `--probe` 双端抓包确认流量实际经过网卡；不能只凭本地地址 ping 成功判断。

**8. 写 hccn.conf**。写入第 1 节选择的 `MF_RUN_FAMILY`；如果选择了 IPv4，此步直接写入 IPv4，无需先运行 IPv6：

```bash
bash "$MF_SCRIPTS/1825-deploy.sh" --env "$MF_ENV" --protocol "$MF_PROTOCOL" --hccn --family "$MF_RUN_FAMILY" --yes && \
  stat -c '%i %n' /etc/hccn.conf && \
  docker exec "$MF_CONTAINER" stat -c '%i %n' /etc/hccn.conf
```

86 选择 IPv4 的回显，inode 数字随环境变化，两边应相同：

```text
[OK] /etc/hccn.conf: 16 个 device 已配置，地址族 ipv4
111255900 /etc/hccn.conf
111255900 /etc/hccn.conf
```

双栈环境需要切换测试地址族时，在宿主机将 `MF_RUN_FAMILY` 改成 `ipv4` 或 `ipv6`，重新执行本步写入对应地址后，再运行第 9 步。例如切换到 IPv4：

```bash
MF_RUN_FAMILY=ipv4
bash "$MF_SCRIPTS/1825-deploy.sh" --env "$MF_ENV" --protocol "$MF_PROTOCOL" --hccn --family "$MF_RUN_FAMILY" --yes
```

单栈环境要测试另一地址族，需先规划并配置该地址族；不能只改 `hccn.conf`。

**9. 容器内跑 MF**

直接使用第 8 步写入 `hccn.conf` 的地址族运行。

执行前确认本步的 CANN 和 run 包环境脚本路径。`MF_DEVICE_PAIRS` 留空时，根据采集结果选择同卡和跨卡组合；也可填写物理 device 组合，例如 `0,1 0,2 0,10`。下面的 `16K`、`64`、`2`、`1` 是小负载测试参数，可在 benchmark 命令中调整。

```bash
MF_ASCEND_ENV=/usr/local/Ascend/ascend-toolkit/set_env.sh
MF_INSTALL_ENV=/usr/local/memfabric_hybrid/set_env.sh
MF_DEVICE_PAIRS=
docker exec -i -e MF_ENV="$MF_ENV" -e MF_ASCEND_ENV="$MF_ASCEND_ENV" -e MF_INSTALL_ENV="$MF_INSTALL_ENV" \
  -e MF_HYBM_RDMA_USE_HCOMM=1 -e MF_DEVICE_PAIRS="$MF_DEVICE_PAIRS" \
  "$MF_CONTAINER" bash -s <<'EOF'
set -e
source "$MF_ASCEND_ENV"
source "$MF_INSTALL_ENV"
source "$MF_ENV"
if [ -n "$MF_DEVICE_PAIRS" ]; then
  read -r -a pairs <<< "$MF_DEVICE_PAIRS"
else
  first=${DEV[0]%%,*}
  pairs=("${DEV[0]}" "$first,${DEV[1]%%,*}")
  if [ "${#DEV[@]}" -gt 5 ]; then pairs+=("$first,${DEV[5]%%,*}"); fi
fi
for pair in "${pairs[@]}"; do
  echo "=== device $pair ==="
  timeout -k 5s 120s mfcli benchmark --device "$pair" --protocol device_rdma --direction all \
    --block-size 16K --batch-size 64 --copy-count 2 --warmup-count 1
done
EOF
```

应退出 0 并打印八方向带宽表。

以下是 86 历史实测带宽块（GB/s）。当时的 MF 包含调试修复，当前环境使用的 run 包需要包含相应修复并重新验证：

```text
| Block | D2rD(GB/s) | D2rH(GB/s) | H2rD(GB/s) | H2rH(GB/s) | rD2D(GB/s) | rH2D(GB/s) | rD2H(GB/s) | rH2H(GB/s) |
| 16K   | 6.789      | 7.354      | 5.232      | 5.520      | 7.602      | 7.060      | 7.684      | 4.242      |
```

86 的 IPv4、IPv6 地址和路由均已配置时，跳过第 5a 步配置，重新采集后复用了路由表 `100`～`107`；前后地址和策略规则一致，双栈抓包通过。使用 CANN 9.2.0、MF 1.5.0（安装包提交 `d00aadda2176477a3bbd2a292c08c88c7fbce21a`），按本步参数验证 `0,1`、`0,2` 和 `0,10` 均通过。以下是 IPv6 `0,10` 的带宽回显节选：

```text
| Block | D2rD(GB/s) | D2rH(GB/s) | H2rD(GB/s) | H2rH(GB/s) | rD2D(GB/s) | rH2D(GB/s) | rD2H(GB/s) | rH2H(GB/s) |
| 16K   | 7.038      | 8.909      | 5.211      | 5.448      | 7.409      | 7.427      | 7.215      | 7.020      |
```

`0,2`、`0,10` 和 40 口的 `12,13`、`14,15`、`12,14` 也有相同参数的历史通过记录；这些都是小负载。双栈环境若要切换地址族，先按第 8 步修改 `hccn.conf`，再重跑本步。86 跳过已有 IP 时，IPv4 `0,10` 的带宽回显节选：

```text
| Block | D2rD(GB/s) | D2rH(GB/s) | H2rD(GB/s) | H2rH(GB/s) | rD2D(GB/s) | rH2D(GB/s) | rD2H(GB/s) | rH2H(GB/s) |
| 16K   | 7.133      | 7.791      | 5.477      | 5.084      | 7.547      | 7.497      | 7.559      | 5.421      |
```

此前从清理状态重建的完整命令和日志保存在 86 宿主机 `/mnt/workspaces/memfabric-1825/runs/1825-doc-audit-20261113-182123/`。其中 `doc-replay-switch-verify-retry.log` 是交换机核对，`doc-replay-host.log` 是主机验证和双端抓包，`doc-replay-mf.log` 是上面的 MF 输出。当时验证后重新清空了本次数据面。

## 4. 可选：清理本次配置

确认不再运行 MF 后，在宿主机执行：

```bash
bash "$MF_SCRIPTS/1825-deploy.sh" --env "$MF_ENV" --protocol "$MF_PROTOCOL" --rollback --yes
```

清理依据状态目录和完全匹配的地址、路由及规则，不对网卡全局地址或路由表做整表 flush；同时恢复配置前的 MTU、RA/autoconf 和所选 device 的 hccn 行。状态目录归档到同目录的 `.completed.<时间>`。交换机 VLAN 必须先检查成员和引用，确认每个 VLAN 仅属于本次目标口且没有其他业务使用。确认后，在宿主机生成清理命令：

```bash
bash "$MF_SCRIPTS/1825-deploy.sh" --env "$MF_ENV" --protocol "$MF_PROTOCOL" --switch-clean > "$MF_OUT_DIR/1825-switch-clean.txt"
cat "$MF_OUT_DIR/1825-switch-clean.txt"
```

再次核对端口和 VLAN 后，将文件内容复制到交换机会话逐行执行。命令将目标口的 PVID 改为 1、关闭端口桥，再删除对应 Vlanif 和 VLAN；QoS 和光模块配置不在清理范围。若 VLAN 或 Vlanif 原本存在且需要保留，或 VLAN 有其他成员或引用，不要执行交换机清理。

## 5. 大包卡住或 PFC 不生效时，怎么对齐 DSCP 和 PFC

完成第 1–3 节且 16K 小包能跑、默认负载卡住或带宽异常时，按本节检查。第 4 节是测试结束后的可选清理，执行本节前不要清理。常见原因是分类和 PFC 落在不同队列：MF 流量进了队列 0，而 PFC 只配在优先级 4。链路是：

```text
MF 的 TC / DSCP
  → 交换机入端口信任方式和映射域
  → 交换机目标口的实际出口队列
  → 交换机目标口 PFC
  → 收发两端网卡的优先级和 PFC
```

下文端口和网卡名是 86 的 `0 → 10` 示例：设备 0 经 `ens100f0` 连 `800GE1/0/37:1`，设备 10 经 `enP1s105f0` 连 `800GE1/0/39:2`。其他环境用第 3.1 节采集的 `NIC`、`SPORT`、`DEV` 和 `hinicadm5 info` 核对。

### 5.1 确认 MF 的 DSCP

当前版本的 `device_rdma` HCOMM 通道使用 `TC=132`、`SL=4`；TC 的高 6 位是 DSCP，因此这里是 DSCP 33。不同版本和程序需以实际使用的值为准。

86 曾在交换机目标口保持 `outer 8021p` 时做对照：小负载八方向通过；试验性代码设置 `TC=236`、`SL=5` 后，跨卡 `0,10` 的默认负载在关闭详细日志的情况下仍于 90 秒超时。测试期间目标口队列 0 计数增长，而队列 4、5 未增长。此对照使用的可配置 TC/SL 代码未合入本分支；这只反映 86 当时的测试结果，不能推断其他交换机或所有参数组合。

本节后续给出 86 实测通过的 `trust dscp` 和 PFC 检查及调整方法。其他现场须先核对映射和端口用途。

### 5.2 可选：手工调整目标交换机口的 trust

`trust upstream dscp-dot1p` 只选择映射域，不会将信任方式改成 DSCP。先由网络管理员确认目标端口用途、映射域及无损队列；改 `trust dscp` 会影响这些端口其他入向流量。86 的 `dscp-dot1p` 将 DSCP 33 映射到 AF4，相关网卡和交换机已对优先级 4 开启 PFC；其他现场不得照搬这个映射。

在宿主机生成只修改采集到的目标端口的命令。检查端口后，把完整输出复制到交换机会话执行：

```bash
source "$MF_ENV"
{
  echo system-view
  for p in "${SPORT[@]}"; do
    echo "interface $p"
    echo 'trust dscp'
    echo commit
    echo quit
  done
  echo return
} > "$MF_OUT_DIR/switch-trust-dscp.txt"
cat "$MF_OUT_DIR/switch-trust-dscp.txt"
```

86 的生成内容节选：

```text
system-view
interface 800GE1/0/37:1
trust dscp
commit
quit
...
return
```

86 交换机执行第一口的实测回显节选：

```text
<Z3-1-B24-GS0-2>system-view
Enter system view, return user view with return command.
[~Z3-1-B24-GS0-2]interface 800GE1/0/37:1
[~Z3-1-B24-GS0-2-800GE1/0/37:1]trust dscp
[~Z3-1-B24-GS0-2-800GE1/0/37:1]commit
[~Z3-1-B24-GS0-2-800GE1/0/37:1]quit
```

在宿主机生成检查命令，复制到交换机逐口执行：

```bash
for p in "${SPORT[@]}"; do
  echo "display qos configuration interface $p"
  echo "display dcb pfc interface $p"
done
```

86 第一口回显节选；计数随现场变化：

```text
<Z3-1-B24-GS0-2>display qos configuration interface 800GE1/0/37:1
 interface 800GE1/0/37:1
 trust flag        : dscp
 diffserv domain   : dscp-dot1p
<Z3-1-B24-GS0-2>display dcb pfc interface 800GE1/0/37:1
Interface         Queue         Received(Frames)
800GE1/0/37:1         4                  2656834
                      5                    29746
```

逐口确认 `trust flag` 和 PFC，并按 5.4 核对实际 DSCP 映射、队列与网卡。若交换机不支持或映射不匹配，先停止并联系网络管理员。

### 5.3 主机：给数据面网卡开 PFC 4

脚本只处理 `NIC` 中整张双端口网卡的两个端口，保留每口原有 PFC 优先级；网口不完整、PFC 未启用或超出设备支持的优先级数量时会停止并要求人工核对。不要把管理口或其他业务网卡加入 `NIC`。先预览再应用，备份文件已存在时脚本不会覆盖：

```bash
source "$MF_ENV"
python3 "$MF_SCRIPTS/1825-pfc.py" "${NIC[@]}" && \
  python3 "$MF_SCRIPTS/1825-pfc.py" --apply --backup "$MF_OUT_DIR/pfc-before.json" "${NIC[@]}"
```

86 已配置后的预览回显，原有优先级 5 得以保留：

```text
hinic0:0 [0, 0, 0, 0, 1, 1, 0, 0] -> [0, 0, 0, 0, 1, 1, 0, 0]
...
[OK] 仅生成计划，未修改网卡。
...
[OK] 已启用 PFC 4，原配置保存在 /mnt/workspaces/memfabric-1825/runs/1825-collect-20261114-112818/pfc-before.json。
```

查询一口，例如 86 的 `hinic0` 端口 0：

```bash
hinicadm5 qos -i hinic0 -t pfc -p 0
```

```text
hinic0 pfc state: enable
pfcup:    0  0  0  0  1  1  0  0
```

需要恢复时，确认测试结束且端口在备份后没有被其他人改动，再按备份逐条执行：

```bash
python3 - "$MF_OUT_DIR/pfc-before.json" <<'PYTHON'
import json
import sys

for item in json.load(open(sys.argv[1], encoding="utf-8")):
    bitmap = ",".join(str(bit) for bit in item["before"])
    print(f"hinicadm5 qos -i {item['card']} -t pfc -p {item['port']} -e 1 -f {bitmap}")
PYTHON
```

### 5.4 核对每一段

交换机用户视图，端口按采集到的 `SPORT` 替换：

```text
display qos configuration interface 800GE1/0/37:1
display diffserv domain name dscp-dot1p
display qos queue statistics interface 800GE1/0/39:2
display dcb pfc interface 800GE1/0/39:2
```

86 回显节选：

```text
interface 800GE1/0/37:1
 trust flag        : dscp
 diffserv domain   : dscp-dot1p

 ip-dscp-inbound 33 phb af4 green

Interface         Queue         Received(Frames)
800GE1/0/39:2         4                     ...
                      5                     ...
```

判据：

- 入端口 `trust flag : dscp`；
- 映射域把 DSCP 33 映射到现场规划的优先级（86 是 AF4）；域名取自入端口回显，不要照抄 `dscp-dot1p`；
- 目标口 PFC 包含队列 4；
- 测试前后 `display qos queue statistics` 的队列 4 计数有增量。

只有 `ip-dscp-inbound 33 phb af4 green` 不能证明报文进了队列 4，还要看实际队列计数；DSCP 33 也不是所有映射域都映射到 AF4。

宿主机查两端网卡（`hinic0:0`、`hinic2:1` 是 86 示例，编号先用 `hinicadm5 info` 核对）：

```bash
hinicadm5 qos -i hinic0 --port_trust q -p 0
hinicadm5 qos -i hinic0 --port_dscp2cos q
hinicadm5 qos -i hinic0 -t pfc -p 0
hinicadm5 qos -i hinic2 --port_trust q -p 1
hinicadm5 qos -i hinic2 --port_dscp2cos q
hinicadm5 qos -i hinic2 -t pfc -p 1
```

86 回显节选，网卡把 DSCP 33 映射到 CoS 4，PFC 4、5 已启用：

```text
port trust: dscp
port dscp2cos:  cos    dscp
                 4     32 33 34 35 36 37 38 39

hinic0 pfc state: enable
pfcup:    0  0  0  0  1  1  0  0
```

测试前后在接收端查 PFC 发送计数：

```bash
ethtool -S enP1s105f0 | grep -E 'mac_tx_pfc(_pri4)?_pkt_num'
```

86 该次对照中 `mac_tx_pfc_pri4_pkt_num` 从 `2658` 增到 `12914`（增量 `10256`），与交换机 `39:2` 收到的 PFC 4 帧增量一致。计数是端口级累计值，其他业务同时经过时可能混入；不要为了比较清空计数。

### 5.5 用 64K 实际流量确认

确认接收端口计数后，在容器内执行（沿用第 9 步的容器和环境脚本变量）：

```bash
docker exec -i -e MF_ASCEND_ENV="$MF_ASCEND_ENV" -e MF_INSTALL_ENV="$MF_INSTALL_ENV" \
  -e MF_HYBM_RDMA_USE_HCOMM=1 "$MF_CONTAINER" bash -s <<'EOF'
set -e
source "$MF_ASCEND_ENV"
source "$MF_INSTALL_ENV"
timeout -k 5 75 mfcli benchmark --device 0,10 --protocol device_rdma --direction D2rH \
  --block-size 64K --batch-size 2048 --copy-count 2 --warmup-count 1 \
  --ip tcp://127.0.0.1:18640 --nic tcp://0.0.0.0:18642 --log
EOF
```

86 实测带宽回显节选，每次约 128 MiB：

```text
| Block | D2rH(GB/s) |
| 64K   | 6.717      |
```

执行结束后重复 5.4 的计数查询。结合队列 4 计数、丢包和 PFC 计数判断分类及拥塞情况；PFC 计数没增加不能单独证明配置错误，没触发反压就不会有 PFC 帧。

### 5.6 验证默认负载

按 5.2–5.4 核对并调整后，先在宿主机使用第 3.3 节第 9 步设置的容器和环境脚本变量。下面是 86 的同卡和跨卡组合；其他环境按实际设备编号修改：

```bash
MF_DEVICE_PAIRS='0,1 0,2 0,10'
docker exec -i -e MF_ASCEND_ENV="$MF_ASCEND_ENV" -e MF_INSTALL_ENV="$MF_INSTALL_ENV" \
  -e MF_HYBM_RDMA_USE_HCOMM=1 -e MF_DEVICE_PAIRS="$MF_DEVICE_PAIRS" \
  "$MF_CONTAINER" bash -s <<'EOF'
set -e
source "$MF_ASCEND_ENV"
source "$MF_INSTALL_ENV"
read -r -a pairs <<< "$MF_DEVICE_PAIRS"
i=0
for pair in "${pairs[@]}"; do
  i=$((i + 1))
  echo "=== device $pair ==="
  timeout -k 8s 180s mfcli benchmark --device "$pair" --protocol device_rdma --direction all \
    --ip "tcp://127.0.0.1:$((18700 + i * 4))" --nic "tcp://0.0.0.0:$((18702 + i * 4))"
done
EOF
```

86 从清空数据面地址和本次 VLAN 的状态重建，交换机八口使用 `trust dscp`，网卡 PFC 4、5 启用；使用 CANN 9.2.0、MF 1.5.0（安装包提交 `d00aadda2176477a3bbd2a292c08c88c7fbce21a`），IPv6 和 IPv4 的 `0,1`、`0,2`、`0,10` 默认负载均退出 0。下面是 IPv6 `0,10` 的 2 MiB 带宽回显节选，单位 GB/s：

```text
| Block | D2rD(GB/s) | D2rH(GB/s) | H2rD(GB/s) | H2rH(GB/s) | rD2D(GB/s) | rH2D(GB/s) | rD2H(GB/s) | rH2H(GB/s) |
| 2M    | 36.361     | 13.578     | 13.485     | 13.495     | 27.005     | 13.489     | 13.578     | 13.489     |
```

双栈环境切换地址族时，先按第 3.3 节第 8 步修改 `/etc/hccn.conf`，再执行上述命令。带宽和计数随现场环境与负载变化。

### 5.7 注意事项

- 需共同核对 MF 的 TC、交换机映射、目标队列 PFC 和网卡 `dscp2cos`；任一侧不匹配时，流量可能落到没有 PFC 的队列。
- 本节不改变 VLAN、地址和路由；第 4 节回滚不恢复交换机 QoS 和网卡 PFC。
- 网卡 PFC 修改的重启持久性未验证，重启后重新查询。
