# 贡献指南

感谢你愿意为这个项目做贡献。请遵循以下约定，让协作更顺畅。

## 工作流程

1. Fork 本仓库并 clone 到本地（或使用你在 SDK 内的工作副本，见 README）。
2. 基于 `main` 创建功能分支：`git checkout -b feat/xxx`。
3. 在分支上修改并提交，commit message 遵循 [Conventional Commits](https://www.conventionalcommits.org/)。
4. 发起 Pull Request，说明改动目的、测试方法（在哪种板子/SDK 版本上验证过）。

## Commit 规范

```
<type>(<scope>): <subject>

<body>
```

- `type`：`feat` / `fix` / `perf` / `refactor` / `docs` / `test` / `chore`
- `scope`：可省略，如 `infer` / `preprocess` / `rtsp` / `encoder` / `cmake`
- 一条 commit 只做一件事。

示例：`fix(infer): 修复 YOLOv5 后处理双重 sigmoid 导致 64 个垃圾框`

## 提交前自查

- 代码中**不要包含**：个人主机绝对路径（`/home/<user>`）、私有 IP、调试残留输出。
- 新增/修改的文件带 Apache-2.0 SPDX 头（参考现有 `rkaiq_rtsp.c` 头部）。
- 不要提交：SDK 闭源 `.so`、模型文件（`.rknn`）、构建产物。运行 `git status` 确认干净后再提交。
- 尽量在板子上验证过再提 PR，并在 PR 描述里注明验证环境（芯片/SDK/rootfs 版本）。

## 版权与许可

- 本项目 Apache-2.0。你贡献的代码默认以 Apache-2.0 授权。
- Rockchip 原版文件保留原 license 头（GPL/BSD 双许可，本项目按 BSD 选项分发）。
- 涉及 gst-rtsp-server（LGPL）的修改请保留其版权头。

## 环境依赖（重要）

本项目**不是独立可构建的**——它依赖 Rockchip SDK（`../rk_aiq` 等）与若干闭源 `.so`。
先阅读 `README.md` 的「构建」和 `dependencies.md`，跑 `scripts/fetch-libs.sh` 补齐依赖，
再尝试编译。编译不通过时先检查依赖是否齐全，再考虑代码问题。
