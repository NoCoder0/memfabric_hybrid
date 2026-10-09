# GitCode 自动 Release 示例

仓库中的 [release.yml](../../.gitcode/workflows/release.yml) 在推送 `v*` tag 时运行：

1. 拉取完整 Git 历史，从版本号较小的最近一个 `vX.Y.Z` tag 到当前 tag 读取主线新增提交。
2. 从 `See merge request: Ascend/memfabric_hybrid!123` 提交信息提取 PR 的来源仓库和编号。
3. 分页读取来源仓库的全部已合入 PR，取得真实标题、作者和合入时间。
4. 直接用 PR 原始标题生成变更条目，按标签和标题关键词在 `## 🚀 What's Changed` 下自动分类，
   每条变更附带 `by @作者 in !PR`，完整版本差异由 `Full Changelog` 提供。
5. 按来源仓库的完整合入历史生成 `## 🌱 New Contributors`，追加 `## Full Changelog`。
6. 生成 `release-notes.md`，创建 GitCode Release；重复运行时若 Release 已存在则跳过创建。

发布目标仓库由 `--repo` 指定，PR 来源由提交正文决定。例如在 `shilinlee_com/memfabric_hybrid`
发布时，来源为 `Ascend/memfabric_hybrid!123` 的 PR 仍会查询上游账号和历史，并链接上游 PR。
这避免 fork 发布时所有变更被误识别为直接提交，进而遗漏作者和首次贡献者。

## 启用

在 GitCode 仓库设置的 Secrets 中创建 `MF_RELEASE_TOKEN`，填入有该仓库 Release 创建权限的个人访问令牌。
Workflow 使用已有的 x86 自托管 Runner 标签和镜像；如实际 Runner 不同，修改 `runs-on` 和 `container`。
工作流文件及脚本需要先进入目标 tag 所指向的提交，然后推送 tag。例如：

```bash
git tag v1.2.1
git push upstream v1.2.1
```

只预览发布说明时，在包含脚本和两个 tag 的工作目录运行；允许 HEAD 与目标 tag 不同：

```bash
python3 script/gitcode_release.py \
  --repo Ascend/memfabric_hybrid \
  --base-tag v1.2.0 \
  --tag v1.2.1 \
  --date 2026-10-01 \
  --output release-note.md
```

公开仓库可匿名预览。私有仓库预览也需要先设置 `GITCODE_TOKEN`。

脚本默认按版本号选择较小的最近一个 tag；若发布线分叉、需要指定比较基线，追加
`--base-tag v1.2.0`。这会比较 `base_tag..tag` 的主线提交。首次发布没有更早 tag 时必须指定基线。

`--date` 未指定时使用北京时间的当前日期。公开仓库的预览不需要发布令牌；`--publish` 要求
`GITCODE_TOKEN`，并验证 checkout 的 HEAD 正好对应目标 tag。

## 自动生成规则

发布说明完全由 Python 脚本根据 Git 历史和 GitCode API 数据生成，不需要人工维护版本内容文件，
也不调用大模型。版本标题、日期和 Installation 由脚本生成；
每条变更直接使用实际 PR 标题、作者和链接，按下面风格展示分类：

```markdown
## 🚀 What's Changed

### Features

- 新增能力 by @作者 in !PR

### Bug Fixes

- 修复说明 by @作者 in !PR

### Performance

- 性能优化 by @作者 in !PR

### Documentation

- 文档更新 by @作者 in !PR

## 🌱 New Contributors

- @作者 made their first contribution in !PR

## Full Changelog

v1.2.0...v1.2.1
```

有对应变更时还会输出 `### Other Changes` 和 `### Compatibility`；没有内容的分类省略。
分类首先使用 `feature`、`fixed`、`performance`、`changed`、`compatibility`、`documentation` 标签。
没有这些标签时使用标题关键词：兼容性及接口移除归入 `Compatibility`，fix/修复等归入
`Bug Fixes`，文档/示例等归入 `Documentation`，perf/性能/带宽等归入 `Performance`，
feat/support/支持等归入 `Features`，其余归入 `Other Changes`。

每个 PR 生成一条说明，作者和 PR 链接保持在同一行 Markdown 中。
分类依据标签和标题，不分析代码 diff，也不自动改写或合并 PR 标题。
纯版本号更新可通过 `Full Changelog` 查看。
缺失的 API 元数据会使生成失败，
CI 日志包含失败原因。已存在的 Release 不会被脚本更新，修改已发布正文需使用平台的编辑功能。

## 执行后仍是平铺格式

只有 `## What's Changed` 和逐条 PR 列表，且没有分类标题时，说明执行的是旧版生成器。
新版开始执行时会在日志中打印 `Using grouped release note format from ... for ...`，包含实际脚本路径。

将当前 `script/gitcode_release.py` 同步到执行预览的工作目录，再运行上面的预览命令。

Tag Workflow 使用 `ref: ${{ atomgit.ref }}` checkout tag 对应提交，因此需要在下一次打 tag 前
合入新版脚本和 workflow 文件。仅修改其他分支上的脚本不会改变已有 tag 的执行内容。
已有 tag 可在包含新版脚本的工作目录中预览，使用 `--tag` 指定比较终点。

## 首次贡献者与范围

`New Contributors` 的定义是“该账号在 PR 来源仓库首次合入 PR，并且该 PR 在本次区间内”。
查询覆盖来源仓库的全部已合入 PR，包含其他分支上的历史贡献。合入时间相同时按 PR 编号排序。
本次没有首次贡献者时省略这一节。直接提交没有可靠的 GitCode 账号映射，因此不会加入这一节。

本例识别主线提交中的 GitCode PR 标记；保留标记的跨仓库 cherry-pick 也能识别其来源。
缺少标记的提交在非空的 `Other Commits` 章节单独列出，并注明 PR 覆盖范围尚未确认。
此方式适用于本仓库保留合入标记的主线历史；改写标记或在普通 merge 中嵌套合入多个 PR 时，
需要进一步核对收录范围。正式发布前请预览生成的内容。

参考：[Tag 触发器](https://docs.gitcode.com/docs/help/home/org_project/pipeline/syntax-reference/trigger-events/)、
[Secrets](https://docs.gitcode.com/docs/help/home/org_project/pipeline/security-permissions/using-secrets/)、
[PR API](https://docs.gitcode.com/docs/apis/get-api-v-5-repos-owner-repo-pulls/)、
[Release API](https://docs.gitcode.com/docs/apis/post-api-v-5-repos-owner-repo-releases/)。
