# 依赖清单

本项目**不是独立可构建的**。它是对 Rockchip `camera_engine_rkaiq/rkisp_demo` 的改造，
构建和运行依赖 Rockchip SDK 提供的库与头文件。请先阅读 `README.md` 与 `CONTRIBUTING.md`。

## 构建依赖

| 组件 | 来源 | 用途 | 获取方式 |
|---|---|---|---|
| `rkaiq`（3A 库 + 头文件） | Rockchip SDK `external/camera_engine_rkaiq/rk_aiq` | ISP 3A 控制 API（`rk_aiq_user_api*.h`） | 需先构建 `../rk_aiq`（默认 installed 目录 `../rk_aiq/build/linux/output/<ARCH>/installed`，可在 CMakeLists 用 `RKAIQ_INSTALLED_DIR` 覆盖） |
| `librknnrt.so` | Rockchip SDK `external/rknpu2`（**闭源**） | NPU 推理运行时 | `./scripts/fetch-libs.sh <SDK_ROOT>` |
| `librga.so` | Rockchip SDK（**闭源**） | RGA 2D 加速（缩放/格式转换/旋转） | `./scripts/fetch-libs.sh <SDK_ROOT>` |
| `libdrm.so` | Rockchip SDK / 系统包 | DRM 显示（libdrm 用户态库） | `./scripts/fetch-libs.sh <SDK_ROOT>`，或系统 `libdrm-dev` |
| `librockchip_mpp.so` | Rockchip SDK `external/mpp`（**闭源**） | 视频硬编解码（MPP H.264） | `./scripts/fetch-libs.sh <SDK_ROOT>` |
| `libgstrtspserver-1.0.so` | gst-rtsp-server（**LGPL-2.1**，开源） | RTSP 服务器 | 系统包 `libgstrtspserver-1.0-0` / 源码构建 1.16.x / SDK 拷贝，见 fetch-libs.sh |
| `gstreamer-1.0` 头文件 | gst-rtsp-server 1.16.3 | `gst/rtsp-server/*.h`（随仓库提交，`demo/libs/include/`） | ✅ 仓库已含，无需获取 |
| 交叉工具链 | 自选 | aarch64 交叉编译 | `gcc-aarch64-linux-gnu` 或 Rockchip SDK 工具链（注意 glibc 与板端 rootfs 兼容，见 `README.md`「构建」） |

## 运行时依赖

| 组件 | 说明 |
|---|---|
| RK3568 平台 | 本工程面向 RK3568（ISP21 + RGA2 + NPU 1T + VOP2 + MPP） |
| 摄像头 sensor | OV5647（MIPI CSI），ISP21 经 AIQ 3A 驱动 |
| YOLOv5 模型 `.rknn` | **不随仓库分发**，需自行转换（见 `model/README.md`） |
| 板端 rootfs | 需匹配交叉工具链的 glibc 版本（如 Ubuntu 18.04 对应 glibc 2.27） |

## 版本参考

本项目在以下环境验证通过（`dependencies.md` 中如与实际 SDK 有出入，以你的 SDK 为准）：

- RK3568 K1Mini 开发板，OV5647，MIPI DSI 屏
- SDK：`rk-linux6.1-2026060111 / rk356x-linux-2026060111`
- 交叉工具链：`gcc-arm-9.2-2019.12-x86_64-aarch64-none-linux-gnu`（glibc 与 Ubuntu 18.04 兼容）
- gst-rtsp-server 1.16.3（`libgstrtspserver-1.0.so.0.1603.0`）
- RKNN-Toolkit2 用于模型量化
