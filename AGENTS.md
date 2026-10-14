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

## 测试

- **全部 UT**：`bash script/run_ut.sh` — 带覆盖率的干净 ASAN 构建（lines≥70%，branches≥40%）。
- **快速（增量，无覆盖率）**：`bash script/run_ut.sh --fast`
- **单个测试**：`bash script/run_ut.sh --fast TestName`（传递 `--gtest_filter=*TestName*`）
- **首次运行前需初始化子模块**：`git submodule update --recursive --init`（获取 `test/3rdparty/googletest` 和 `test/3rdparty/mockcpp`）。
- **测试范围**：文档修改检查内容、路径和链接；代码修改先运行相关 UT，存在跨模块影响时再扩大范围。
  用户要求或 CI 要求的检查仍须完成；已通过的检查仅在新增变更、失败或未解决问题需要时重复。
- **快速模式清理**：`--fast` 在首次构建或构建配置指纹变化时仍会清理当前 worktree 的 `build/` 和 `output/`。

## 已知问题（aarch64 / devcontainer）

- **aarch64 上 ASAN 不可用**：`run_ut.sh` 默认 `ASAN`，但在 aarch64（含 Ubuntu 22.04）上 ASAN 与 mockcpp 钩子机制冲突（`stack-use-after-return` 误报、入口插桩 SEGV）。本地迭代改用 `MF_UT_BUILD_TYPE=DEBUG bash script/run_ut.sh`。

- **AccLinksTest 端口 8100**：测试硬编码 `LISTEN_PORT=8100`（`test/ut/testcase/acc_links/acc_links_test.cpp:37`）；devcontainer 宿主机若占用 8100，该 suite 54 个用例会在 `SetUp` 集体失败（`bind` EADDRINUSE）。本地可换端口或换独立 netns。

## 预提交 / 代码风格

- **安装**：`pip install pre-commit && pre-commit install --install-hooks`
- **CI PR 检查**：`bash script/ci-pre-commit-pr.sh`
- **Python**：ruff 目标 `py310`，行长度 120。配置位于 `pre-commit/pyproject.toml`。
- **C++**：clang-format v18.1.8，基于 Google，IndentWidth=4，ColumnLimit=120，Allman 大括号。配置位于 `.clang-format`。

## 代码规范（强制要求）

编写代码务必遵守[编码规范和安全规范](docs/zh/contributions/coding_standards)及以下规范，违反将导致代码被拒绝：

### 单一职责原则

- 每个函数只完成一个明确功能
- 避免"万能函数"（如同时做初始化+计算+输出）
- 函数名应清晰表达其单一职责

### 日志打印规范

- 新增错误返回路径必须有可追溯的 ERROR 级别日志；仅透传已记录错误时按下述规则处理
- 根错误发生处必须记录失败原因和关键参数，确保日志能定位问题
- 上层仅透传下层已记录过的错误时，可不强制重复记录 ERROR 日志
- 禁止只返回错误而完全没有任何一层记录根因日志
- 选择关键参数时必须贴合本项目语义，禁止照搬本项目不存在的泛业务 ID 示例
- 新增或修改日志必须遵守[MemFabric日志与文档规范](docs/zh/contributions/coding_standards/docs_specification.md)和 [MemFabric C++ 安全编码规范](docs/zh/contributions/coding_standards/cpp-secure.md) 中的 LOG API 安全使用规则。优先使用仓库现有日志宏；失败路径日志需包含错误描述、关键上下文、外部 API 名称、返回码和必要参数；日志内容使用英文，避免语法/拼写错误并为耗时、大小、带宽等度量信息标明单位；禁止记录密码、密钥、token、个人信息等敏感数据；性能敏感数据面和高频循环内不得持续打印 INFO/DEBUG 或重复错误日志；格式化占位符的数量、顺序和类型必须与参数一致。

关键参数优先级：

| 优先级 | 参数类型 | MemFabric 示例 |
|--------|---------|----------------|
| 高 | 标识符 | `rankId`、`localRank`、`remoteRank`、`deviceId`、`pid`、`threadId`、`channel`、`handle` |
| 高 | 资源名/资源句柄 | `shmName`、`shmKey`、`storeKey`、`bufferId`、`memHandle`、队列名、文件名 |
| 高 | 通信/任务定位信息 | `traceId`、`requestId`、`jobId`、`batchTag`、`opId`（仅在代码中真实存在时） |
| 中 | 路径/端点 | 文件路径、`ASCEND_MF_STORE_URL`、etcd endpoint、TCP/HCOM/RDMA 地址和端口 |
| 中 | 配置项 | 超时时间、重试次数、阈值、`worldSize`、`rankSize`、`opType`、`xpu_type`、HCOM/RDMA 开关 |
| 中 | 外部调用目标 | ACL/URMA/HCOM/etcd 接口名、下游服务名、RPC 方法名、`smem_bm_*` / `hybm_*` 调用点 |
| 中 | 状态码/错误码 | `ret`、`errno`、`StoreErrorCode`、`BM_*` 错误码、ACL/HCOM/URMA 错误码、退出码 |
| 低 | 内部中间值 | `idx`、`offset`、`batchSize`、`len`/`length`、`srcVA`/`dstVA`（仅在有助于定位时） |

### 行宽限制

- 行宽≤120字符
- 超长需合理换行，在运算符后截断
- 换行后后续行与运算符对齐

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
| 官方应用 | `app/zbal/`、`app/tensorRTL/` | — |
| 示例 | `examples/` | — |

## 产物 / 依赖

- **前置依赖**：CMake ≥3.12（推荐 3.20+），GCC ≥11.4，GLIBC ≥2.28，Ninja/Make 4.3+，Python ≥3.8，pybind11 ≥2.10.3。
- **可选依赖**：libibverbs-dev（HCOM RDMA）、Go（etcd 后端）、auditwheel（Python wheel）。
- **Python wheel 构建**：需设置 `MEMFABRIC_VERSION`；`XPU_TYPE` 追加 cpu/gpu 标签；`IS_MANYLINUX` 控制平台标签。
- **忽略生成产物**：`build/`、`output/`、`compile_commands.json`、`wheels/`、shared libs — 这些是构建产物，非源码。

## 许可证

Mulan PSL v2 — 参见文件头部和 LICENSE 文件。
