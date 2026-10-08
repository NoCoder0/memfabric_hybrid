# 贡献指南

欢迎参与 MemFabric Hybrid！无论是修复代码、补充测试，还是完善文档和分享使用反馈，都能帮助项目改进。
参与项目讨论和贡献时，请遵守[社区行为准则](./CODE_OF_CONDUCT.md)。

## 从哪里开始

你可以通过以下方式贡献：

- 修复缺陷、改进内存管理或传输实现。
- 补充单元测试、回归测试和异常路径验证。
- 完善 `docs/` 下的文档，修正文档中的命令、路径和链接。
- 补充或改进 `examples/` 下的 C++、Python 样例。
- 分享性能实验和应用集成反馈，并记录环境、复现步骤及测量结果。

先查看 [GitCode Issues](https://gitcode.com/Ascend/memfabric_hybrid/issues)，搜索是否已有相关问题。
首次参与可以关注 `good-first-issue` 标签，并结合自己的环境和经验选择任务。
认领前阅读问题描述、评论和关联 PR，在 Issue 下说明拟开展的工作，避免重复投入。
较大的功能、公共 API 或架构改动，请先通过 Issue 与维护者讨论使用场景、方案和验证方式。

报告问题时，请提供预期行为、实际行为和最小复现步骤，注明项目版本或提交、操作系统与架构，以及相关的 Python、CANN、驱动版本、设备和传输类型。
涉及跨节点运行时，补充节点数、rank 配置及必要日志。
分享日志前请移除口令、令牌、私钥和个人信息。

## 环境与硬件要求

根据贡献内容选择环境，详细依赖和配套要求见[编译安装](./docs/zh/installation.md)及 [README](./README.md)。

| 贡献或验证内容 | 环境要求 |
| --- | --- |
| 文档、代码阅读和评审 | 任意操作系统即可，无需 NPU 或专用网卡 |
| 单元测试 | Linux 开发环境和测试依赖；UT 使用模拟设备接口，不能替代真机验证 |
| 无加速器构建和 CPU 相关验证 | 使用 `--xpu_type NONE`；根据传输方式准备通信组件和网络环境 |
| NPU 内存访问、设备传输和相关样例 | 对应昇腾设备、CANN、驱动及所需互联环境 |
| Host RDMA / Host URMA 和跨节点传输 | 对应网卡、驱动、通信库及所需节点；RDMA 编译还需开发依赖 |

UT 环境可使用以下多 Python 容器镜像：

- ARM：`swr.cn-north-4.myhuaweicloud.com/memfabric-hybrid/memfabric-hybrid_arm:multi_python_v2`
- x86：`swr.cn-north-4.myhuaweicloud.com/memfabric-hybrid/memfabric-hybrid_x86:multi_python_v2`

`XPU_TYPE=NONE` 表示不使用 NPU/GPU，并不表示所有功能都不需要通信组件或专用网卡。
例如 Host RDMA、Host URMA 的真机验证仍依赖对应硬件；跨节点验证也需要准备相应节点和网络。
UT 的模拟接口验证与实际设备、网卡及多节点验证应分别记录。

可参考 [DevContainer 快速入门](./docs/zh/devcontainer_quickstart.md)准备远端昇腾开发环境。
该文档中的设备样例运行流程需要对应硬件，不作为普通 Linux 贡献的前置条件。
运行样例前，按[环境变量说明](./docs/zh/environment_variables.md)配置所需环境。
相关配置包括存储地址 `ASCEND_MF_STORE_URL`，以及 NPU 场景的 `ASCEND_HOME_PATH` 或 GPU 场景的 `CUDA_HOME`。

## 获取源码与构建

在 GitCode Fork 仓库后克隆自己的 Fork，按任务对应版本选择目标分支，再创建工作分支。

不确定目标分支时，请参考[分支策略][branch-policy]。

首次运行 UT 或构建测试工具前，在仓库根目录初始化子模块：

```bash
git submodule update --recursive --init
```

构建优先使用封装脚本，参数和安装步骤见[编译安装](./docs/zh/installation.md)：

```bash
# 默认：RELEASE、Python ON、NPU
bash script/build_and_pack_run.sh

# NPU 调试构建
bash script/build_and_pack_run.sh --build_mode DEBUG

# 无加速器，仅构建项目库和 Python 绑定，不启用 HCOM
bash script/build_and_pack_run.sh --xpu_type NONE --build_hcom OFF --build_hcom_rdma OFF

# 无加速器，启用 HCOM 并关闭其 RDMA 支持
bash script/build_and_pack_run.sh --xpu_type NONE --build_hcom ON --build_hcom_rdma OFF
```

需要 RDMA 时启用 `--build_hcom ON --build_hcom_rdma ON`，并准备 `libibverbs-dev` 等依赖；
需要 Host URMA 时按环境启用 `--build_hcom ON --build_hcom_ub ON`，并准备对应依赖。
启用 HCOM 时，构建会获取通信库源码，请确保能够访问相关依赖仓库。
构建配置不等于已完成相应硬件上的运行验证。

## 验证改动

### 单元测试与覆盖率

代码修改先运行相关 UT，存在跨模块影响时扩大测试范围。
首次运行需准备构建及测试依赖。
当前 UT 脚本启用 HCOM 和 RDMA 编译支持，需准备 `libibverbs-dev` 并能获取通信库源码。
完整覆盖率流程还需 `lcov` / `genhtml`。命令详情见[UT 使用指南](./script/run_ut.md)。

构建、UT 和 Fuzz 脚本已内置 `dos2unix` 兼容处理，未安装时会使用 `sed` 转换换行，无需另外安装或手动定义函数。

常用 UT 命令：

```bash
# 快速运行全部 UT，不生成或检查覆盖率
bash script/run_ut.sh --fast

# 运行匹配 SmemBmTest 的相关用例
bash script/run_ut.sh --fast SmemBmTest

# 全量干净构建、UT 和覆盖率检查
bash script/run_ut.sh
```

完整模式默认使用 ASAN，要求行覆盖率至少 70%、分支覆盖率至少 40%。
`--fast` 适合本地增量迭代，不执行覆盖率检查。
首次构建或配置指纹变化时，仍会清理当前工作目录的 `build/` 和 `output/`。
需要完整验证时请使用默认模式。

aarch64 上 ASAN 与 mockcpp 钩子机制存在兼容性问题，本地迭代使用 DEBUG：

```bash
MF_UT_BUILD_TYPE=DEBUG bash script/run_ut.sh --fast
```

使用 DEBUG 运行完整模式时仍检查覆盖率。其他已知环境问题见 [UT 使用指南](./script/run_ut.md)。

### 格式与预提交检查

在仓库根目录安装并启用预提交检查：

```bash
pip install pre-commit
pre-commit install --install-hooks
```

可针对改动文件运行 `pre-commit run --files <文件路径>`，全量检查入口为：

```bash
bash script/ci-pre-commit-pr.sh
```

全量脚本会检查和格式化整个仓库；若自动修改文件，请查看 diff，确认改动并重新检查。
配置见[预提交配置](./.pre-commit-config.yaml)和[预提交指南](./pre-commit/README.md)。

文档修改检查内容准确性、示例命令、仓库内路径和链接，并运行适用的预提交检查。
代码、API、样例或性能改动应补充相应验证；需要硬件但尚未完成的验证，请明确说明所缺环境和待验证内容。
用户或 CI 要求的检查仍需完成。

## 代码与文档规范

遵循周围代码的风格和现有依赖，详细项目要求见 [AGENTS.md](./AGENTS.md)。

- C/C++ 使用仓库的 [.clang-format](./.clang-format) 和 clang-format 18.1.8，缩进为 4 个空格，行宽不超过 120 字符。
- Python 使用 [Ruff 配置](./pre-commit/pyproject.toml)，目标版本为 `py310`，行宽不超过 120 字符。
- 函数保持单一职责，新增和修改的函数尽量不超过 50 行非空、非注释代码；控制块嵌套深度不超过 4 层。
- 新增错误返回路径应在根因发生处记录 ERROR 日志，包含失败原因和相关 rank、设备、资源、端点或错误码。
  上层仅透传已经记录的错误时，可以避免重复日志。
- 公共 API、参数或环境变量发生变化时，同步更新相应文档、样例和验证。
- 中文文档放在 `docs/zh/`，优先链接现有说明，避免重复维护大段安装步骤。

接口参考见 [API 简介](./docs/zh/feature.md)、[C API](./docs/zh/API.md)和 [Python API](./docs/zh/pythonAPI.md)。
代码跳转配置见 [IDE 代码跳转指南](./docs/zh/ide_code_navigation_guide.md)。

## 提交与 Pull Request

一个 PR 聚焦一个逻辑改动，提交说明清楚表达改动目的；较大的工作可拆成独立、可评审的步骤。
提交前检查 diff，避免带入与任务无关的修改、敏感信息，以及 `build/`、`output/`、`wheels/` 等生成产物。

按照现有 [GitCode PR 模板](./.gitcode/PULL_REQUEST_TEMPLATE.md)填写说明和开发自检项，至少包含：

- 改了什么、为什么改，以及关联 Issue、里程碑或具体使用场景。
- 修改方案、影响范围，必要时说明兼容性和性能影响。
- 是否涉及 UT/ST；不需要时说明理由，需要时提供对应测试。
- 实际执行的验证命令、环境和结果；未完成的硬件验证应明确列出。
- 接口、参数、环境变量或行为发生变化时，对应资料已同步更新。

性能改动请提供同一环境下的前后对比、测量方法和必要配置，使结果可以复现。
提交后持续跟进评审反馈，解释取舍或按反馈更新实现，并在新增改动影响验证结果时重新运行相关检查。
讨论聚焦事实和技术问题，使用清晰、尊重的语言。

代码贡献遵循项目的 [Mulan PSL v2 许可证](./LICENSE)。
社区行为准则采用的许可见其[贡献归属](./CODE_OF_CONDUCT.md#贡献归属)。

[branch-policy]: https://gitcode.com/Ascend/memfabric_hybrid/wiki/%E5%88%86%E6%94%AF%E7%AD%96%E7%95%A5.md

返回[项目首页](./README.md) · 阅读[社区行为准则](./CODE_OF_CONDUCT.md)
