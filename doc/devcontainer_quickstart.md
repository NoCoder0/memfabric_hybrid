# MemFabric Hybrid — 远端开发 & 全量运行指南

## 前置条件

- **远端服务器**：昇腾 NPU 服务器, 已经安装了npu驱动
- **本地机器**：VS Code + Remote—SSH 扩展
- **网络**：本地能 SSH 到远端服务器，远端能拉 gitcode + Docker 镜像

---

## 1. 远端克隆项目

SSH 登录远端服务器，克隆仓库：

```bash
git clone https://gitcode.com/Ascend/memfabric_hybrid.git
cd memfabric_hybrid
```

---

## 2. VS Code SSH-Remote 连接

不需要手动在远端启动 VS Code Server，VS Code 会自动处理。

**方式 A — 命令行快捷连接**（本地终端执行）：

```bash
code --remote ssh-remote+<你的服务器地址> /path/to/memfabric_hybrid
```

例如：

```bash
code --remote ssh-remote+192.168.1.100 /home/user/memfabric_hybrid
```

**方式 B — VS Code UI**：

1. `F1` → `Remote-SSH: Connect to Host...`
2. 输入 `ssh user@host` 或从 `~/.ssh/config` 选择
3. 输入服务器密码
4. 连接后 `File > Open Folder...` → 选择 `memfabric_hybrid` 目录
5. 输入服务器密码

> VS Code 会自动在远端安装 `vscode-server`，首次连接需等待几十秒。

---

## 3. 在 Dev Container 中打开

1. 确保远端已安装 Docker（root 或 docker 组权限）
2. VS Code 中 `F1` → `Dev Containers: Reopen in Container`
3. 输入服务器密码
4. 等待镜像拉取 + 容器构建 + postCreateCommand 完成

第一次会拉取 `quay.nju.edu.cn/ascend/vllm-ascend:v0.20.2rc1-a3`（约 15-20 GB），耗时较长。
> 官方镜像源为 quay.io/ascend/vllm-ascend:v0.22.1rc1-a3， 配置中使用中国国内镜像加速下载

postCreateCommand 会自动做：
- 初始化 `test/3rdparty/` 子模块（googletest + mockcpp）
- 安装 pip 依赖（pre-commit, pytest 等）
- CMake 冒烟测试

完成后 VS Code 左下角显示 `Dev Container: MemFabric Dev Container`。
> 点击右下角的 show logs 会在终端显示构建细节和进度

---

## 4. 运行全部 Examples

### 参数说明

| 参数 | 默认 | 说明 |
|------|------|------|
| *(无)* | — | Python + C++ 全量运行 |
| `--python` | — | 仅运行 Python 例子 |
| `--cpp` | — | 仅运行 C++ 例子（自动构建+安装运行包） |
| `--store-url` | `tcp://127.0.0.1:8570` | 配置存储地址 |
| `--start-store` | off | 脚本自动启动 store（退出自动关闭） |
| `--continue-on-error` | on | 单个失败继续跑 |
| `--example-timeout` | 180s | 单用例超时秒数 |
| `--dry-run` | off | 只打印不执行 |
| `--verbose` | off | 显示用例完整输出 |
| `--run-transfer` | off | 启用传输用例（需双节点） |
| `--run-multi-node` | off | 启用多节点用例（需双节点） |

### 使用示例

```bash
# 查看使用指南（不实际执行）
bash script/run_all_examples.sh --help

# 查看会跑哪些例子（不实际执行）
bash script/run_all_examples.sh --dry-run

# 全量运行（Python + C++，自动构建运行包）
bash script/run_all_examples.sh

# 只跑 Python examples
bash script/run_all_examples.sh --python

# 只跑 C++ examples
bash script/run_all_examples.sh --cpp

# 运行时查看详细输出
bash script/run_all_examples.sh --verbose
```

---

## 常见问题

**Q: 拉镜像太慢 / 超时**
使用代理或国内 mirror，在远端 `/etc/docker/daemon.json` 配置 registry mirror。

**Q: 构建时找不到 `pybind11`**
`postCreateCommand` 已安装，如跳过则手动：

```bash
pip install pybind11
```

**Q: 没有 NPU 能不能跑**
不能。MemFabric 依赖昇腾 NPU 驱动（`davinci`, `devmm_svm`）。无 NPU 环境下构建会失败。

**Q: 只想用 Python 不用 C++ 例子**
`run_all_examples.sh` 默认全跑（含 C++），用 `--python` 只跑 Python。
