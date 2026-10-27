# 官方 CLI 安装与认证

## 检测与自动安装

在任务实际执行的主机或容器中检测 `gitcode`；本机安装不代表远端已有。
命令存在且 `gitcode version` 成功时复用，并继续能力验证。命令缺失或无法启动时自动安装，
不再询问是否安装。已存在的隔离安装可运行时也可以直接复用。

默认使用[官方 PyPI 包 `gitcode-cli`](https://pypi.org/project/gitcode-cli/)，从
`https://pypi.org/simple/` 安装当前 Python 可用的最新稳定版。先确认 Python ≥3.8，
在 Git 私有目录创建专用虚拟环境，避免工具和依赖进入源码提交或覆盖系统安装。
任务指定 CLI 版本时使用对应的精确版本，而不是自行升级。

Linux / macOS，在已确认的仓库根目录执行；可用解释器名称不同时替换 `python3`：

```bash
cli_env=$(git rev-parse --git-path agent-tools/gitcode-cli)
python3 -m venv "$cli_env"
"$cli_env/bin/python" -m pip --isolated install --disable-pip-version-check --no-input \
  --index-url https://pypi.org/simple/ gitcode-cli
export PATH="$(cd "$cli_env/bin" && pwd):$PATH"
hash -r
```

Windows PowerShell，使用实际可用的 `py -3` 或 `python` 创建虚拟环境；每一步成功后再继续：

```powershell
$cliEnv = git rev-parse --git-path agent-tools/gitcode-cli
if ($LASTEXITCODE -ne 0) { throw "Cannot locate Git private directory" }
py -3 -m venv $cliEnv
if ($LASTEXITCODE -ne 0) { throw "Cannot create CLI environment" }
$cliPython = Join-Path $cliEnv "Scripts/python.exe"
& $cliPython -m pip --isolated install --disable-pip-version-check --no-input `
  --index-url https://pypi.org/simple/ gitcode-cli
if ($LASTEXITCODE -ne 0) { throw "Cannot install official CLI" }
$cliBin = (Resolve-Path (Join-Path $cliEnv "Scripts")).Path
$env:Path = "$cliBin;$env:Path"
```

POSIX shell 同样逐步检查退出码，安装失败时不要运行后续命令。
PATH 只对本次进程生效；后续工具调用需显式携带该 PATH，或直接使用安装目录内 `gitcode` 的绝对路径。
不要依赖 shell 变量在多次工具调用间保留，也不修改 shell 启动文件或全局 PATH。

Python、venv 或 pip 不可用，或所选版本未同步到 PyPI 时，按
[官方安装说明与 Release](https://gitcode.com/atomgit-cli/cli)选择匹配操作系统和架构的官方安装方式。
可复用已有 npm，执行官方 `atomgit-cli` bootstrap 并使用专用安装目录及不修改持久 PATH 的选项；
先查看其 help 核对当前参数。也可使用官方 Release wheel 或平台二进制，核实版本和官方校验值。
不要安装 npm 上的裸名 `gitcode-cli`，它不是官方 CLI。
如果网络、依赖或平台限制仍导致安装失败，说明尝试过的方式和具体阻断，继续可完成的本地准备；
不绕过证书校验、不自动修改系统包管理配置，也不把安装问题误报为认证失败。

## 验证后继续

安装后用刚安装的可执行文件执行以下命令，全部成功才恢复原 PR 流程：

```bash
gitcode version
gitcode schema "pr create"
gitcode schema "pr list"
```

核对实际可执行文件路径、版本及 schema 中的 `--base`、`--head`、`--fork`、`--body-file`、
`--json` 和分页参数，确保没有误用 PATH 中的其他同名程序。
CLI 支持时还可用 `gitcode doctor install --json` 核对安装来源；不支持该诊断不算安装失败。

## 认证失败时请求 TOKEN

首次远端 API 调用前执行 `gitcode auth status --json`。本地显示已登录不保证有目标仓库权限，
仍需在原流程中用目标仓库查询或 PR 列表核实服务端响应。
未登录、TOKEN 缺失或无效、API 明确返回认证失败或权限不足时，停止依赖认证的步骤并明确告知用户：

> GitCode 认证失败或权限不足，请提供有权访问目标仓库并创建 PR 的有效 TOKEN，
> 或在当前运行环境配置 `GITCODE_TOKEN` / `GC_TOKEN` 后告知我继续。

认证来源优先级为 `GC_TOKEN` → `GITCODE_TOKEN` → 本地登录配置。
已有无效 `GC_TOKEN` 时，仅新增 `GITCODE_TOKEN` 不会覆盖它；定位正在使用的来源，不打印变量值。
用户提供 TOKEN 后通过进程环境或标准输入传递，再验证认证和目标仓库访问，成功后从中断处继续。
只在确需持久登录且用户已授权时使用 `gitcode auth login --with-token`，TOKEN 通过标准输入传入。
不将 TOKEN 写入命令参数、shell 历史、remote URL、源码、skill、PR 正文或日志；
不调用 `auth token` 或 `auth status --show-token` 回显凭据。

网络超时、DNS、TLS 或服务端故障应按实际原因报告，不能仅凭非零退出码要求更换 TOKEN。
创建请求失败后仍遵守原流程的查重与结果核实规则，不能因重新认证而盲目重复创建。
