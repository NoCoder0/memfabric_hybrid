# AGENTS

## 构建

- **默认**：`bash script/build_and_pack_run.sh` → RELEASE，Python ON，XPU_TYPE=NPU。
- **Debug**：`bash script/build_and_pack_run.sh --build_mode DEBUG`
- **无加速器**：`bash script/build_and_pack_run.sh --xpu_type NONE --build_hcom ON --build_hcom_rdma OFF`
- **HCOM + RDMA**：`--build_hcom ON --build_hcom_rdma ON`
- **etcd 后端**：`--build_etcd_backend ON`
- **Bazel**：`--build_tool bazel`（CMake 为默认）。
- 优先使用 wrapper 而非 `script/build.sh`（位置参数易混淆）。build.sh 默认值：
  BUILD_OPEN_ABI=OFF（旧 C++11 ABI），BUILD_PYTHON=ON，ENABLE_PTRACER=ON，XPU_TYPE=NPU，
  HCOM/etcd 禁用；MF_BUILD_JOBS 默认取 CPU 数的 60%（向下取整，最低 1），可通过同名环境变量覆盖。

## Commit 和 PR 标题

使用约定式提交[（Conventional Commits）](https://www.conventionalcommits.org/zh-hans/v1.0.0/)风格的提交信息和 PR 标题：`type(scope): summary`。

类型包括 `feat`、`fix`、`perf`、`docs`、`chore`、`refactor`、`build`、`ci`、`style` 和 `test`。
作用域（scope）可选，使用受影响的模块，例如 `hybm`、`smem`、`transport`、`entity` 或 `benchmark`。
摘要简洁描述最终变更及其效果，避免仅写“修复问题”或“优化代码”。

示例：`fix(smem): reconnect after heartbeat timeout`、`docs: clarify hardware support`、
`perf(transport): reduce redundant synchronization`。

PR 正文按 [.gitcode/PULL_REQUEST_TEMPLATE.md](.gitcode/PULL_REQUEST_TEMPLATE.md) 填写，关联 issue，
说明问题或需求、修改方案、影响范围及 UT/ST 验证结果；不涉及 UT/ST 时说明理由，未执行的检查不得标记为通过。
涉及公共 API、参数、环境变量或 ABI 的变更，说明兼容性影响并同步相关资料；
性能变更提供可复现的测试命令、硬件与软件环境及同等条件下的前后对比数据，注明尚未完成的验证。

## 代码规范（强制要求）

编写代码务必遵守[编码规范和安全规范](docs/zh/contributions/coding_standards)及以下规范，违反将导致代码被拒绝：

### 一般原则

- 除非可组合或可重用，否则请将所有内容放在一个函数中。
- 在外部输入边界一次完成校验，将明确类型的 C++ 数据传入内部，不重复验证类型、构造函数或上游校验已保证且保持不变的条件，同时保留外部调用失败及动态状态的必要检查。
- 不要预先提取一次性使用的辅助函数。除非辅助函数会被重用、隐藏了真正复杂的边界，或者具有清晰独立的名称且能改进调用者，否则应在调用点内联逻辑。
- 在为推测性的或极不可能发生的竞态或安全极端情况增加复杂性之前，请向用户解释具体的故障模式、可能性和复杂性成本，并获得他们的认可。不要为了提高理论鲁棒性而悄悄扩大范围。
- 尽量避免try/catch。
- 当初始化表达式已清楚表达类型或显式类型冗长时优先使用 auto，公共接口及涉及数值范围、单位或资源语义的场景保留明确类型，以可读性和类型正确性为准。
- 外面进来的数据只在入口校验一次，往里传就用定好类型的值，不要每一层再判断一遍。
- 新增错误返回路径必须有可追溯的 ERROR 级别日志，必须记录失败原因和关键参数，确保日志能定位问题；仅透传已记录错误时可不强制要求。
- MUST: 不要增加注释，除非有容易产生误解的行为。

### 复杂逻辑

当函数包含较多校验分支或辅助细节时，让主函数清楚呈现正常执行流程，在调用处检查错误并尽早返回，
将确有独立含义的解析、校验或准备逻辑提取为小型辅助函数。

以下 C++ 示例省略类型定义和辅助函数实现；辅助函数在失败处记录错误，主函数检查并透传返回码：

```cpp
Result RequireConfig(const Input &input, Config &config);
Result ReadMetadata(const Input &input, Metadata &metadata);
Result CreateThing(const Config &config, const Metadata &metadata, Thing &thing);

Result LoadThing(const Input &input, Thing &thing)
{
    Config config{};
    auto ret = RequireConfig(input, config);
    if (ret != BM_OK)
    {
        return ret;
    }

    Metadata metadata{};
    ret = ReadMetadata(input, metadata);
    if (ret != BM_OK)
    {
        return ret;
    }

    return CreateThing(config, metadata, thing);
}
```

- 辅助函数靠近其服务的代码，在满足 C++ 声明顺序且有助于阅读时，可将实现放在主函数之后。
- 不要将简单表达式拆成大量仅使用一次的辅助函数；仅在能表达配置解析、元数据读取等独立概念并改善调用者可读性时提取。
- 同步解析、校验和选项构造保持为普通同步函数，不为封装形式引入异步任务、回调或 `std::future`。
- 解析不可信 JSON 字符串时，优先复用仓库已有的解析与校验工具，遵循现有错误处理约定，避免重复编写解析及异常转换逻辑。
- 注释用于解释不明显的约束和容易产生误解的行为，不重复描述显而易见的赋值或控制流程。

## 测试

- **全部 UT**：`bash script/run_ut.sh` — 带覆盖率的干净 ASAN 构建（lines≥70%，branches≥40%）。
- **快速（增量，无覆盖率）**：`bash script/run_ut.sh --fast`
- **单个测试**：`bash script/run_ut.sh --fast TestName`（传递 `--gtest_filter=*TestName*`）
- **首次运行前需初始化子模块**：`git submodule update --recursive --init`（获取 `test/3rdparty/googletest` 和 `test/3rdparty/mockcpp`）。
- **测试范围**：文档修改检查内容、路径和链接；代码修改先运行相关 UT，存在跨模块影响时再扩大范围。
  用户要求或 CI 要求的检查仍须完成；已通过的检查仅在新增变更、失败或未解决问题需要时重复。
- **快速模式清理**：`--fast` 在首次构建或构建配置指纹变化时仍会清理当前 worktree 的 `build/` 和 `output/`。

## 预提交

- **准备或创建 GitCode PR**：使用 [gitcode-pr-create](.agents/skills/gitcode-pr-create/SKILL.md)，
  按贡献指南和 PR 模板核对目标分支、验证证据及源分支远端提交。

- **安装**：`pip install pre-commit && pre-commit install --install-hooks`
- **CI PR 检查**：`bash script/ci-pre-commit-pr.sh`
- **Python**：ruff 目标 `py310`，行长度 120。配置位于 `pre-commit/pyproject.toml`。
- **C++**：clang-format v18.1.8，基于 Google，IndentWidth=4，ColumnLimit=120，Allman 大括号。配置位于 `.clang-format`。

## 已知问题（aarch64 / devcontainer）

- **aarch64 上 ASAN 不可用**：`run_ut.sh` 默认 `ASAN`，但在 aarch64（含 Ubuntu 22.04）上 ASAN 与 mockcpp 钩子机制冲突（`stack-use-after-return` 误报、入口插桩 SEGV）。本地迭代改用 `MF_UT_BUILD_TYPE=DEBUG bash script/run_ut.sh`。

- **AccLinksTest 端口 8100**：测试硬编码 `LISTEN_PORT=8100`（`test/ut/testcase/acc_links/acc_links_test.cpp:37`）；devcontainer 宿主机若占用 8100，该 suite 54 个用例会在 `SetUp` 集体失败（`bind` EADDRINUSE）。本地可换端口或换独立 netns。

## 运行时 / 配置

- **配置存储 URL**（`ASCEND_MF_STORE_URL`）必填：`tcp://...`、`etcd://...` 或 `reg://...`。
- **NPU**：`ASCEND_HOME_PATH` 必须指向 CANN/Ascend 安装目录。
- **GPU**：`CUDA_HOME` 必须指向 CUDA 安装目录。
- **Python 日志级别**：`ASCEND_MF_LOG_LEVEL`（0-4）。
- 完整环境变量列表：`docs/zh/environment_variables.md`。

## 仓库模块边界

| 模块 | 路径 | 产物 |
|------|------|------|
| SMEM API (BM/TRANS/SHM) | `src/smem/` | `libmf_smem.so` |
| 内存管理 / 传输 | `src/hybm/` | `libmf_hybm_core.so` |
| IPC 控制通道 | `src/acc_links/` | — |
| etcd Go 后端 | `src/util/etcd_client/etcd_store_backend/` | shared lib |
| 公共 C 头文件 (SMEM) | `src/smem/include/host/` | — |
| 公共头文件 (hybm) | `include/hybm/` | — |
| 示例 | `examples/` | — |

## 产物 / 依赖

- **前置依赖**：CMake ≥3.12（推荐 3.20+），GCC ≥11.4，GLIBC ≥2.28，Ninja/Make 4.3+，Python ≥3.8，pybind11 ≥2.10.3。
- **可选依赖**：libibverbs-dev（HCOM RDMA）、Go（etcd 后端）、auditwheel（Python wheel）。
- **Python wheel 构建**：需设置 `MEMFABRIC_VERSION`；`XPU_TYPE` 追加 cpu/gpu 标签；`IS_MANYLINUX` 控制平台标签。
- **忽略生成产物**：`build/`、`output/`、`compile_commands.json`、`wheels/`、shared libs — 这些是构建产物，非源码。

## 许可证

Mulan PSL v2 — 参见文件头部和 LICENSE 文件。
