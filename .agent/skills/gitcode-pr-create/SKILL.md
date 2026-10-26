---
name: gitcode-pr-create
description: 为当前代码仓准备或创建 GitCode PR，缺少 CLI 时自动安装官方版本并验证，认证失败时提示提供 TOKEN，按仓库模板核实分支、验证证据及已有 PR 后创建。
---

# GitCode PR 创建

适用于当前 checkout，支持同仓和 Fork PR。使用官方 `gitcode` CLI，无需工作区脚本、机器配置或其他 skill。
流程吸收了[官方创建 PR skill](https://gitcode.com/atomgit-cli/skills/tree/main/gitcode-pr-create)的 CLI 入口，
并补充远端提交核验、查重和本仓贡献要求。

## 0. 准备 CLI 与认证

在已确认的目标 checkout 根目录检测 `gitcode` 是否可用。缺失或无法启动时，
按[安装与认证](references/cli-setup.md)自动安装官方版本，
无需再次询问是否安装；验证版本、实际命令路径及 PR 子命令后，从中断处继续执行。
已有可用 CLI 时直接复用，不自动升级。安装失败时报告具体原因，不能跳过验证后继续调用 CLI。

首次远端 API 调用前检查认证；流程中任何阶段出现未登录、TOKEN 无效或权限不足，按同一参考文档
明确告知用户提供有效 TOKEN。等待期间可以继续本地草稿整理，不能执行依赖认证的操作。
环境准备不会把“只准备 PR”的请求扩展成创建 PR。

## 1. 确认请求与仓库

- 先读仓库根目录的 `AGENTS.md`、存在的 `CLAUDE.md`、`CONTRIBUTING.md` 和 `.gitcode/PULL_REQUEST_TEMPLATE.md`。
  代码或文档改动遵循 `docs/zh/contributions/coding_standards/` 中相关规范。
- 只要求准备 PR 时，完成正文和命令预览；只有创建 PR 的请求才执行创建命令。
  commit、push、rebase 按已有授权执行；只要求 commit 时只做本地提交。已有授权不重复询问。
- 确认操作目录是选定 checkout 的 Git 根目录，避免把不存在的子目录误当父仓库。
  检查分支、暂存区和未提交改动；保留用户改动，不自动 stash、丢弃或混入提交。

```bash
git rev-parse --show-toplevel
git status --short --branch
git branch --show-current
git log -5 --oneline
git diff --cached --stat
git diff --stat
git ls-files --others --exclude-standard
gitcode version
gitcode schema "pr create"
gitcode schema "pr list"
```

使用 `gitcode`，避免 Windows PowerShell 的 `gc` 别名冲突。命令参数以已安装 CLI 的 schema/help 为准；
CLI 缺失时先执行自动安装流程。沿用 Git 身份和凭据管理方式，不改全局配置，
不添加虚构共同作者，不输出 token、私钥或含凭据的 remote URL。

## 2. 确定源与目标

从用户上下文、remote 和 Fork 元数据确认以下值，再把它们赋给示例中的 shell 变量；不要原样运行占位符。

| 变量 | 含义 |
| --- | --- |
| `source_remote` | 源分支推送的远端，通常为 `origin` |
| `source_url` | 所选远端实际 push URL；用于核实源仓库与远端提交 |
| `source_repo` | 源仓库 `owner/repo`，从实际 push URL 解析，去掉 `.git` |
| `target_remote` | 目标仓库的已配置 remote 名或无凭据 URL，用于只读查询和 fetch |
| `target_repo` | 目标仓库 `owner/repo` |
| `branch` | 本地具名源分支，与本次提交一致 |
| `base` | 任务或版本要求的目标分支，必须显式传入 CLI |

有 `upstream` 时核实其用途；缺失时查询源仓库的 Fork parent 或使用用户指定目标，不直接把 `origin` 当上游。
只有无法从现有信息确定且会改变提交去向时才询问。可以用 `gitcode repo view "$source_repo" --json`
查看仓库摘要；若摘要未包含 Fork parent，用 `gitcode api "repos/$source_repo"` 读取原始元数据中的 `parent`。
字段以实际输出为准。源远端有多个 push URL 时，先明确使用哪个。

目标分支依据任务、Issue、贡献指南中的分支策略和远端信息确定；远端默认分支只是线索。
不要固定为 `main`、`master` 或 `develop`，也不要仅凭仓库名称相同判断 Fork 关系。
新功能分支通常用 `codex/<主题>`，用户指定名称优先；detached HEAD 需先确定具名源分支。

```bash
git check-ref-format --branch "$branch"
git check-ref-format --branch "$base"
git ls-remote --symref "$target_remote" HEAD
git fetch --no-tags "$target_remote" "refs/heads/$base"
target_commit=$(git rev-parse FETCH_HEAD)
git log --oneline "$target_commit..$branch"
git diff --stat "$target_commit...$branch"
git diff --check "$target_commit...$branch"
git diff "$target_commit...$branch"
```

查看完整 diff，确认有实际变更且范围符合任务。准备未提交改动时，还需读取暂存区、工作树及未跟踪文件，
将它们纳入审查和规模统计；`git diff --stat` 不包含未跟踪文件，不能仅凭它判断变更规模。
若源分支是目标主线，将功能分支列入准备方案，实际提交时选用或建立适当的功能分支。
不擅自变基、改写历史或强推。若目标版本的贡献要求或 PR 模板与当前 checkout 不同，以目标版本为准。
仅准备且目标尚未确定时，先输出已有证据、草稿和待定参数，不运行依赖这些未知参数的命令。

## 3. 收集验证证据

- 根据 diff 运行相关检查，记录命令、环境、结果及未完成项；已有适用证据可以复用。
- 预提交检查使用 `.pre-commit-config.yaml`。优先 `pre-commit run --files <改动文件路径>`；
  CLI 支持时可用 `gitcode precommit check --run --yes`，先确认其范围。
  全量检查或 CI 要求时使用 `bash script/ci-pre-commit-pr.sh`，注意它会检查并可能格式化整个仓库。
  工具缺失时在隔离环境准备依赖；hook 修改文件后查看 diff、处理修改并重新检查，不跳过失败。
- 代码变更按 `AGENTS.md` 运行相关 UT，需要时扩大范围。当前入口为
  `bash script/run_ut.sh --fast <测试过滤>`，完整覆盖率入口为 `bash script/run_ut.sh`。
  构建使用 `script/build_and_pack_run.sh`，参数先查看当前脚本。
- 纯文档或 skill 变更检查内容、命令、路径、链接及适用的预提交项，无需强制构建或硬件测试。
  mock UT、原生构建与 NPU、RDMA/URMA、HCOM、多节点真机验证分别描述，不能互相替代。
  性能结论提供同环境前后对比和可复现配置，不编造测试或测量结果。

## 4. 写正文

把正文放在 Git 私有目录，避免随源码提交；创建目录后将路径保存到 `body_file`：

```bash
body_dir=$(git rev-parse --git-path agent-pr)
mkdir -p "$body_dir"
body_file="$body_dir/body.md"
```

按 `.gitcode/PULL_REQUEST_TEMPLATE.md` 的原有章节写入该文件，不另造一套通用模板。
说明问题或功能、修改方案、影响范围和关联 Issue/里程碑；未关联时如实标明，不能伪造链接。
UT/ST 不适用时说明理由；需要时列出已执行验证及待完成项。
逐项核实开发自检中的变更规模、反合标题、资料同步、对外接口/参数/环境变量校验和三方依赖要求，
仅勾选有证据支持的项目。标题围绕最终改动，反合按模板注明 `【反合】`。
正文使用实际换行；不要把单次任务的正文、token 或环境凭据固化到 skill。

## 5. 发布前核验与查重

先确保本次改动已按授权提交，工作树和暂存区已核对；有未提交改动时继续准备草稿，不声称它们会进入 PR。
源分支如需推送，在已有授权范围内推送指定分支。随后直接查询实际 push URL：

```bash
gitcode auth status
git rev-parse "refs/heads/$branch"
git ls-remote --exit-code "$source_url" "refs/heads/$branch"
gitcode pr list -R "$target_repo" --state open --base "$base" --paginate --per-page 100 --json
```

本地 SHA 与远端源分支 SHA 必须一致；缺失或不一致时先处理，不能直接创建。
读取完整分页结果，按源仓库身份、源分支和目标分支一起匹配，不能只比较分支名。
列表请求失败、响应格式不明或分页不完整时先查清，不能当成“没有已有 PR”。
找到同源同目标 PR 就返回已有链接；修改正文或状态按用户授权执行，不重复创建。

## 6. 创建与返回结果

将已写好的标题赋给 `title`，核对正文与 diff 后，按源目标关系选择一个命令。
用户要求草稿 PR 时加 `--draft`；这里的命令是正式创建，不能当作 dry-run 执行。

```bash
# 同仓 PR
gitcode pr create -R "$target_repo" --base "$base" --head "$branch" \
  --title "$title" --body-file "$body_file" --json

# Fork PR
gitcode pr create -R "$target_repo" --fork "$source_repo" --base "$base" --head "$branch" \
  --title "$title" --body-file "$body_file" --json
```

解析 JSON 中的实际 PR 编号与链接，并只读查看该 PR，确认源仓库、源分支和目标分支正确。
创建超时、网络中断或结果缺少链接时，先查询 PR 列表或网页核实；状态不明确时停止再次创建，不盲目重试。
工具支持将 PR 附加到当前聊天时附加真实链接。
最终给出链接、源与目标、验证结果和待完成项；只准备时给出正文路径、创建命令和缺失条件。
