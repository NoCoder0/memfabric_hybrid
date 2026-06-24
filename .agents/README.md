# MemFabric Agent Skills

本目录保存面向 MemFabric 仓库的通用 Agent skills。每个 skill 都是一个独立目录，核心文件为 `SKILL.md`。

## Skills 列表

| Skill | 适用场景 | 示例用法 |
|---|---|---|
| `memfabric-code-review` | 代码审查、错误返回日志、代码规范、UT/test 字面量、行宽和函数长度检查 | `使用 memfabric-code-review 检查这次变更` |
| `memfabric-release` | 生成 MemFabric Hybrid changelog / release note，并列出本次 release 新增 PR | `使用 memfabric-release 生成 1.2.1 release note` |

## 使用方式

- 需要某个能力时，在请求中显式写出 skill 名称，例如：`使用 memfabric-release ...`。
- 不确定该用哪个 skill 时，描述任务目标即可；Agent 应根据 `SKILL.md` 的 `description` 自动选择最匹配的 skill。
- 一个任务可以组合多个 skill，例如发布前可同时使用 `memfabric-code-review` 和 `memfabric-release`。
- 修改或新增 skill 时，只编辑对应目录下的 `SKILL.md`；本仓库不保留 `openai.yaml` 等平台专用 UI 元数据。

## 免责声明

- Skills 是 Agent 的工作指南，不是自动化测试或质量保证的替代品。
- 涉及硬件、NPU、RDMA、UB、HCOM、多节点、etcd、性能数据的结论，必须以实际环境验证结果为准。
- Release、benchmark、集成适配等流程中，未运行的验证项必须在最终结论中明确说明，不能默认视为通过。
- Skills 中的命令和路径基于当前仓库结构维护；仓库脚本、目录或构建参数变化时，应同步更新对应 `SKILL.md`。
- 对外接口、环境变量、安装方式、发布内容等变更仍需人工 review，不能仅凭 skill 说明直接发布。
