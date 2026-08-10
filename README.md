# RK3568 端侧 AI 视觉流水线（rkisp-demo 扩展）

基于 Rockchip [camera_engine_rkaiq/rkisp_demo](https://github.com/rockchip-linux/camera_engine_rkaiq)
改造的全硬件加速端侧 AI 视觉流水线：**MIPI 采集 → ISP 3A → RGA 预处理 → NPU 推理 → 画框显示 → MPP 编码 → RTSP 推流**。

在 RK3568（K1Mini）+ OV5647 + MIPI DSI 上验证通过，检测帧率 **15.8fps**。

## 特性

- **采集**：MIPI CSI → ISP21（AIQ 3A 自动曝光/白平衡）→ V4L2 **DMABUF 零拷贝**出图（1280×800@15fps）
- **AI 推理**：RGA 硬件预处理（NV12→RGB888 缩放 + letterbox，约 2.5ms）→ RKNN NPU 运行 YOLOv5 → 检测框逆映射回相机坐标
- **显示**：检测框画回 1280×800 相机主图后，DRM KMS **原子接口（Atomic API）**非阻塞提交**完整 1280×800 画面**（RGA 旋转 270° 适配 MIPI DSI 竖屏 800×1280）
- **推流**：送显前的同一主图经 **RGA copy** 缩放 → MPP H.264 硬编码 → GStreamer RTSP 服务器，VLC/ffplay 实时预览
- **零拷贝贯通**：相机 DMA → RGA → NPU → MPP 编码全链路共享 dma-buf，无 CPU 搬移
- **性能可观测**：全链路 perf 埋点，逐阶段统计耗时与帧率

## 架构

```
┌────────────────────────── RK3568 ───────────────────────────┐
│                                                              │
│  OV5647 (MIPI CSI)                                            │
│    │  RAW                                                    │
│    ▼                                                         │
│  ISP21 + AIQ 3A (AE/AWB)  ──▶  mainpath NV12 1280×800 (dma-fd)│
│    │                                                         │
│    ├─── AI 推理路 ────────────────────────────────────────────│
│    │     RGA 缩放+格式转换 (NV12→RGB888 640×640, letterbox)    │
│    │           │                                             │
│    │           ▼                                             │
│    │     RKNN NPU (YOLOv5) 检测框                             │
│    │           │ 检测框逆映射回相机坐标                          │
│    │           ▼                                             │
│    │    画框回写 → 写回 1280×800 相机主图 (DMA_BUF_SYNC)       │
│    │                                                         │
│    ├─── 显示路（送显前同一相机主图，已含检测框）─────────────────│
│    │     RGA 旋转 270° (1280×800 → 800×1280)                  │
│    │           │                                             │
│    │           ▼                                             │
│    │     DRM KMS atomic 非阻塞提交 → MIPI DSI 屏 (800×1280)    │
│    │                                                         │
│    └─── 推流路（RGA copy 送显前相机主图，已含检测框）────────────│
│          RGA 缩放 (1280×800 → 640×480)                        │
│                │                                             │
│                ▼                                             │
│          MPP H.264 硬编码 → GStreamer RTSP                     │
│                                              │                 │
└──────────────────────────────────────────────┼─────────────────┘
                                               ▼
                                     rtsp://<device_ip>:8554/live
```

三条支路共享**同一份 1280×800 相机主图 dma-buf**：AI 路只读它做缩放推理并**把检测框画回主图**；
显示路与推流路读取的均是画框后的主图（显示分辨率即相机 1280×800，旋转 270° 适配竖屏，并非 640×640 的模型输入）。

## 性能数据（板端 100 帧平均实测 log）

> 优化前数据取自**未优化前的性能测试 log**（AI 检测流 + RTSP 推流同时运行，100 帧平均）。

| 数据流 | 处理模块 | 优化前 (ms) | 优化后 (ms) | 手段 |
|---|---|---|---|---|
| **AI 检测流** | **总耗时 / 帧率** | **100.85 / 10.5fps** | **60.52 / 15.7fps** | 零拷贝 + atomic 显示等（见下） |
| | RKNN YOLOv5 推理 | 80.96 | 53.85 | 零拷贝推理输入（rknn_create_mem_from_fd） |
| | VOP 显示（含 RGA 270° 旋转） | 17.01 | 3.43 | DRM atomic 非阻塞提交（SetPlane 10.42ms→0.24ms） |
| | RGA NV12→RGB 预处理（1280×800→640×640） | 2.78 | 2.50 | letterbox 灰边懒填充 |
| **推流/录像流** | **总耗时 / 帧率** | **15.42 / 10.5fps** | **8.25 / 15.7fps** | MPP 零拷贝编码 |
| | RGA NV12→NV12 缩放（1280×800→640×480） | 4.51 | 4.61 | 持平 |
| | MPP H.264 编码 | 10.50 | 3.20 | 零拷贝输入（ION import） |
| | RTSP 推送 | 0.41 | 0.43 | 持平 |

> 注：NPU 提频 600→900MHz 后 RKNN 推理可进一步降至 45.52ms，但帧率已近源帧率上限（相机 15fps），
> 无法继续提升。**默认 600MHz 不提频即可达 15.7fps**（实测 15.7~15.8fps）。

## 目录结构

```
├── demo/                     # 源码（Rockchip 原版 + 本项目扩展）
│   ├── rkisp_demo.cpp        # 主程序：V4L2 采帧 + YOLO + DRM 显示
│   ├── rkaiq_preprocess.*    # RGA 预处理（NV12→RGB 640×640 + letterbox）
│   ├── rkaiq_infer.*         # RKNN 零拷贝推理 + 后处理
│   ├── rkaiq_encoder.*       # MPP H.264 硬编码线程
│   ├── rkaiq_rtsp.*          # GStreamer RTSP 服务器
│   ├── drmDsp.*              # DRM KMS 原子显示
│   ├── postprocess*.*        # YOLOv5 后处理（来自 rknn_model_zoo）
│   ├── libs/                 # 头文件随仓库；.so 由脚本从 SDK 获取
│   └── include/              # RGA/DRM/RKNN 等头文件
├── cmake/                    # 构建配置（Rockchip 原版）
├── scripts/
│   ├── build.sh              # 交叉编译（环境变量可覆盖）
│   └── fetch-libs.sh         # 从本地 Rockchip SDK 拷贝闭源 .so
├── model/README.md           # YOLOv5 模型获取与转换说明
├── dependencies.md           # 依赖清单（版本与来源）
├── NOTICE.md                 # 组件许可清单
├── LICENSE                   # Apache-2.0
└── README.md
```

## 构建

### 1. 依赖

- Rockchip SDK（含 `../rk_aiq`，需先构建出 installed 目录）
- 交叉工具链（推荐 `gcc-arm-9.2-2019.12-x86_64-aarch64-none-linux-gnu`，**glibc 版本需与板端 rootfs 兼容**）
- CMake ≥ 2.8

### 2. 补齐闭源库

```bash
# 在 SDK 内使用时
./scripts/fetch-libs.sh ~/rk-linux6.1-2026060111/rk356x-linux-2026060111
```

### 3. 编译

```bash
./scripts/build.sh
# 或自定义工具链：
# TOOLCHAIN_ROOT=/path/to/toolchain ./scripts/build.sh
```

产物：`build_local/exe/Release/rkisp_demo`

## 部署与运行

1. 拷贝可执行文件与 `model/` 到板子，`LD_LIBRARY_PATH` 指向 `demo/libs/arm64`：
   ```bash
   ./rkisp_demo --device /dev/video0 --width 1280 --height 800 --count 2000 --vop \
                --model ./model/yolov5s-640-640.rknn
   ```
2. VLC/ffplay 拉流查看：
   ```
   rtsp://<device_ip>:8554/live
   ```
   低延迟观看：`ffplay -fflags nobuffer rtsp://<device_ip>:8554/live`

## 主要优化点

1. **修复检测失效**：后处理对 YOLOv5 输出重复 sigmoid 导致 64 个垃圾框、帧率仅 0.2fps；对照
   `rknn_model_zoo` 源码修复后恢复 ~10.5fps（即上表性能基线）。
2. **预处理提速**：CPU letterbox 23ms → RGA 硬件 2.5ms。
3. **显示提速 80%**：`drmModeSetPlane`（同步阻塞 ~10.4ms）→ DRM atomic 非阻塞提交（0.24ms）。
4. **NPU 提频（可选）**：devfreq 600→900MHz，推理 53.85→45.52ms；默认 600MHz 即可跑满 15.7fps，
   无需提频。
5. **零拷贝贯通**：采集→RGA→NPU→编码全链路 dma-buf，无 CPU 搬移。

## 已知限制

- 依赖 Rockchip SDK 与闭源库，**不能脱离 SDK 独立构建**（见 `dependencies.md`）。
- 仅验证 RK3568 + OV5647 + Ubuntu 18.04 rootfs（glibc 2.27）。
- 模型不随仓库分发（见 `model/README.md`）。

## License

- 本项目 Apache-2.0（见 [LICENSE](LICENSE)）。
- 各组件许可与归属见 [NOTICE.md](NOTICE.md)。
- 提交前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。
