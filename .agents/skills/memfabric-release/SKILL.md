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

生成前先确认：

- `VERSION`：目标版本，例如 `1.2.1`。
- `YYYY-MM-DD`：发布日期；用户未指定时使用当前日期。
- 对比范围：上一版本 tag/commit 到当前版本 tag/commit，或用户提供的 PR/commit 列表。
- 本次 release 所有新增 PR 的编号、链接和标题。
- 是否已有用户整理的重点变更；如有，优先使用用户整理内容，并用 git/PR 差异补充遗漏。

如果缺少对比范围或 PR 列表，应先通过本地 git、远端 PR 信息或用户补充来获得；无法确认时在结果中明确标注信息缺失。

## 生成原则

- 只包含有变化的章节；空章节必须省略。
- 内容应面向用户，描述行为变化、使用影响和兼容性影响，不要机械罗列 commit。
- 同一条变更只放入最合适的一个章节，避免重复。
- 保留必要的技术名词、API 名称、环境变量、命令、路径和 PR 编号。
- 不确定分类时，优先放入 `Changed`，并在需要时说明依据。
- `References` 必须列出本次 release 所有新增 PR。

## 分类规则

- `Summary`：本次 release 的整体摘要，概括最重要的能力、修复和影响。
- `Feature`：新增能力、新 API、新运行模式、新平台支持、新工具或新集成。
- `Changed`：已有行为、配置、构建、性能、日志、默认值、内部实现或非 bug 修复类调整。
- `Fixed`：bug 修复、稳定性修复、错误处理修复、崩溃/hang/timeout/数据错误修复。
- `Compatibility`：ABI/API、Python 版本、硬件/驱动/CANN、依赖、包名、安装方式或行为兼容性说明。
- `Documentation`：文档、示例、README、安装说明、API 说明、release 文档更新。
- `References`：本次 release 所有新增 PR。

## 输出格式

严格使用以下格式。除 `Installation` 和 `References` 外，仅包含有变化的章节；没有变化的章节不要输出。

```markdown
# MemFabric Hybrid v{VERSION} - {YYYY-MM-DD}

## Installation

- PyPI: [memfabric-hybrid](https://pypi.org/project/memfabric-hybrid/)
- Install: `pip install memfabric-hybrid`

## Summary

Summary of this release

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

## References

- [!728](https://gitcode.com/Ascend/memfabric_hybrid/pull/728) — PR 标题
```

## References 规则

`References` 中列出本次 release 所有新增 PR，格式必须为：

```markdown
- [!728](https://gitcode.com/Ascend/memfabric_hybrid/pull/728) — PR 标题
```

要求：

- PR 按编号升序或合入时间排序；同一份 release note 中保持一种排序方式。
- PR 标题使用实际标题，不要改写到失真。
- 如果 PR 链接不可用，但编号和标题可确认，仍按标准格式输出链接。
- 如果某个变更来自直接 commit 而非 PR，应在 `References` 后补充说明，不要伪造 PR。

## 最终检查

输出前确认：

- 标题版本和日期正确。
- 空章节已删除。
- `Installation` 中 PyPI 链接和安装命令保持固定格式。
- `References` 覆盖本次 release 所有新增 PR。
- 每条变更描述都能对应到 PR、commit 或用户提供的发布信息。
