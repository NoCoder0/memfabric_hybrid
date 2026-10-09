---
name: memfabric-release
description: "生成 MemFabric Hybrid changelog 或 release note 时使用。只负责根据版本、日期、提交/PR 差异和用户提供的变更信息整理发布说明，不负责发布检查、构建矩阵、产物验证或实际 release 执行。"
---

# MemFabric Release Note

## 适用范围

此技能只用于生成 MemFabric Hybrid 的 changelog / release note。

不负责：

- 创建或维护发布检查清单。
- 修改版本号或发布分支。
- 构建 run 包、wheel 或发布产物。
- 执行发布、上传 PyPI、创建 tag 或创建 release。
- 判断未运行的测试、硬件门禁或构建矩阵是否通过。

## 需要收集的信息

优先从用户已有上下文、本地 git 和可用的远端 PR 信息中核实以下内容，不重复询问已提供的信息：

- `VERSION`：目标版本，例如 `1.2.1`。
- `YYYY-MM-DD`：发布日期；用户未指定时使用当前日期。
- 对比范围：上一版本 tag/commit 到当前版本 tag/commit，或用户提供的 PR/commit 列表。
- 本次 release 所有新增 PR 的编号、链接和标题。
- 是否已有用户整理的重点变更；如有，优先使用用户整理内容，并用 git/PR 差异补充遗漏。

仅在目标版本或对比范围无法可靠确定、且会改变收录内容时询问用户；同时继续整理已有依据的内容。
若 PR 信息不完整，输出基于已知范围的草稿，注明信息缺失和覆盖范围尚未确认，不声称已收录全部 PR。

## 生成原则

- 除固定保留的 `Installation` 外，只包含有变化的章节，空章节必须省略。
- 内容应面向用户，描述行为变化、使用影响和兼容性影响，不要机械罗列 commit。
- 同一条变更只放入最合适的一个章节，避免重复。
- 保留必要的技术名词、API 名称、环境变量、命令、路径和 PR 编号。
- 不确定分类时，优先放入 `Changed`，并在需要时说明依据。
- 在变更条目中附上相关 PR 链接，使用 `Full Changelog` 链接完整版本差异。

## 分类规则

- `Feature`：新增能力、新 API、新运行模式、新平台支持、新工具或新集成。
- `Changed`：已有行为、配置、构建、性能、日志、默认值、内部实现或非 bug 修复类调整。
- `Fixed`：bug 修复、稳定性修复、错误处理修复、崩溃/hang/timeout/数据错误修复。
- `Compatibility`：ABI/API、Python 版本、硬件/驱动/CANN、依赖、包名、安装方式或行为兼容性说明。
- `Documentation`：文档、示例、README、安装说明、API 说明、release 文档更新。
- `Full Changelog`：上一版本到当前版本的完整差异链接。

## 输出格式

严格使用以下格式。除 `Installation` 外，仅包含有变化的章节；没有变化的章节不要输出。

```markdown
# MemFabric Hybrid v{VERSION} - {YYYY-MM-DD}

## Installation

- PyPI: [memfabric-hybrid](https://pypi.org/project/memfabric-hybrid/)
- Install: `pip install memfabric-hybrid`

## Feature

- Description of added new feature

## Changed

- Description of change

## Fixed

- Description of fix

## Compatibility

- Description of compatibility change

## Documentation

- Description of docs changes

## Full Changelog

[v{PREVIOUS_VERSION}...v{VERSION}](https://gitcode.com/Ascend/memfabric_hybrid/compare/v{PREVIOUS_VERSION}...v{VERSION})
```

## 最终检查

输出前确认：

- 标题版本和日期正确。
- 无变化的章节已删除；`Installation` 按输出格式保留。
- `Installation` 中 PyPI 链接和安装命令保持固定格式。
- `Full Changelog` 指向正确的版本比较区间；信息不完整时已明确标为草稿，并注明已知范围及缺失项。
- 每条变更描述都能对应到 PR、commit 或用户提供的发布信息。
