---
date: 2026-11-16
slug: first-contribution-guide
description: 从认领 Issue 到代码合入，完成你的第一次 MemFabric Hybrid 开源贡献。
categories:
  - 贡献指南
tags:
  - MemFabric Hybrid
  - 开源贡献
  - 新手入门
---

# 第一次给 MemFabric Hybrid 提 PR：从认领 Issue 到代码合入

感谢你参与 MemFabric Hybrid 开源贡献！无论是修复问题、完善文档还是补充测试，你的参与都能帮助项目变得更好。

如果你是第一次参与，可以跟着这篇文章完成一次贡献：先认领 Issue，再准备代码、开发和检查，最后创建 PR 并跟进评审。

<!-- more -->

## 查找并认领 Issue

打开 [Issue 列表](https://gitcode.com/Ascend/memfabric_hybrid/issues)，找一个自己感兴趣的任务。先阅读问题描述和已有讨论，看看是否已经有人在处理。

找到合适的任务后，在 Issue 下留言，说明你希望认领。如果你对解决方案有想法，也可以一起写出来，和维护者讨论。

**等待维护者将 Issue 分配给你，确认分配后再开始开发。**

## 准备开发环境和代码

开发环境取决于你的贡献内容。修改文档不需要 NPU；如果要验证 NPU 或其他硬件功能，就需要准备对应设备和环境。昇腾开发环境可以参考 [DevContainer 快速入门](https://memfabric-hybrid.readthedocs.io/zh-cn/latest/wiki/developer_guide/development_environment/devcontainer_quickstart/)。

在 GitCode 上 Fork [MemFabric Hybrid 仓库](https://gitcode.com/Ascend/memfabric_hybrid)，再把自己的 Fork 克隆到本地。

下面以 `master` 为目标分支。实际开发时，请根据 Issue 要求选择目标分支。

```bash
# 克隆个人 Fork
git clone <个人-Fork-地址>
cd memfabric_hybrid

# 获取社区仓库的最新代码
git remote add upstream https://gitcode.com/Ascend/memfabric_hybrid.git
git fetch upstream

# 从 master 创建工作分支
git switch -c my-change upstream/master
```

将 `my-change` 替换为能描述本次工作的分支名。

## 开发和本地检查

开始开发前，可以先阅读[贡献指南](https://memfabric-hybrid.readthedocs.io/zh-cn/latest/wiki/developer_guide/contributing/)和 [AGENTS.md](https://gitcode.com/Ascend/memfabric_hybrid/blob/master/AGENTS.md)，了解项目规范。然后按照 Issue 中讨论的方案进行修改。

如果修改了代码，请运行相关单元测试。第一次运行前，需要初始化子模块：

```bash
git submodule update --recursive --init

# 快速运行相关测试，将 SmemBmTest 换成你的测试名称
bash script/run_ut.sh --fast SmemBmTest

# 需要完整验证时，运行全部 UT 和覆盖率检查
bash script/run_ut.sh
```

提交前，运行预提交检查：

```bash
pip install pre-commit
pre-commit install --install-hooks

# 检查本次改动的文件
pre-commit run --files <文件路径>
```

如果修改的是文档，请检查内容、命令和链接，并运行适用的预提交检查。如果暂时缺少某项验证所需的环境，创建 PR 时说明未完成的检查和原因。

## 提交代码并创建 PR

先检查改动，确认没有带入无关文件，然后提交并推送到自己的 Fork：

```bash
git status
git diff
git add <文件路径>
git commit -m "fix(smem): describe the change"
git push -u origin my-change
```

第一次创建 PR 前，请按照 [CLA 使用指南](https://gitcode.com/Ascend/infrastructure/blob/master/docs/cla/cla%E4%BD%BF%E7%94%A8%E6%8C%87%E5%8D%97.md)签署 CLA。

接着在 GitCode 上创建 PR：

1. 选择你的工作分支作为源分支，选择 Issue 要求的社区分支作为目标分支。
2. 检查文件改动，按 PR 模板关联 Issue，填写修改内容和实际验证结果。
3. 保留 **Squash 合并**选项。

## 跟进评审

PR 创建后，关注流水线结果和评审意见。收到反馈后及时回复；需要修改时，继续在原工作分支修改并推送。确认意见已处理后，将对应讨论标记为已解决。

等待检查通过和维护者批准，PR 就可以按社区流程合入了。如果 PR 不再需要合入，请说明原因并关闭。
