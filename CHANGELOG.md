# Changelog

本项目基于 Rockchip `camera_engine_rkaiq/rkisp_demo` 改造，新增端侧 AI 视觉推理与 RTSP 推流能力。

## [Unreleased]

### 板端实测回归修复（15.8fps 恢复）
- **回归**：开源整理时误删 CMakeLists 中无条件的 `-DISPDEMO_ATOMIC_NONBLOCK=1`
  （当时误判为重复行）。因构建走 LOCAL_DRM_LIB 分支（`libdrm_FOUND` 为假），
  `if(libdrm_FOUND)` 内的该宏不生效，回退到同步 `drmModeSetPlane`，帧率从 15.8fps 掉到 12fps。
- **修复**：恢复无条件定义并加注释说明。
- **验证**：板端实测，NPU 600MHz（不提频）下 atomic 显示 + 其他优化即达
  **15.7~15.8fps**（AI 流总耗时约 50-59ms）；提频到 900MHz 推理再降但帧率受源帧率限制不再提升。

### 开源前的整理（本次提交）
- **可移植性**：CMakeLists 移除绝对路径，gst-rtsp-server 头文件随项目提供，`demo/libs` 相对路径构建。
- **可配置性**：新增 `--model <path>` 命令行参数，模型路径默认 `./model/yolov5s-640-640.rknn`，不再硬编码板端路径。
- **清理**：移除调试残留输出与私有 IP/用户名信息；为用户新增模块补齐 Apache-2.0 SPDX 头。
- **新增仓库配套**：`LICENSE`（Apache-2.0）、`NOTICE.md`（组件与许可清单）、`CONTRIBUTING.md`、`CHANGELOG.md`、`scripts/build.sh`、`scripts/fetch-libs.sh`、`dependencies.md`。

### 既有功能（追溯到本项目开发期间）
- 采集：MIPI CSI → ISP21（AIQ 3A）→ V4L2 DMABUF 零拷贝出图。
- AI 推理：RGA 硬件预处理（NV12→RGB888 + letterbox）→ RKNN NPU 跑 YOLOv5 → 检测框回写。
- 显示：DRM KMS 原子接口驱动 MIPI DSI 竖屏，RGA 硬件旋转 270°。
- 推流：RGA 缩放 → MPP H.264 硬编码 → GStreamer RTSP 推流。
- 性能优化：定位修复后处理双重 sigmoid（0.2fps→15.8fps）、RGA 预处理替代 CPU letterbox（23ms→2.5ms）、DRM atomic 非阻塞提交、NPU 提频至 900MHz。
