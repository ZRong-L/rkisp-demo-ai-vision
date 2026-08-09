# RK3568 端侧 AI 视觉流水线（rkisp-demo 扩展）

基于 Rockchip [camera_engine_rkaiq/rkisp_demo](https://github.com/rockchip-linux/camera_engine_rkaiq)
改造的全硬件加速端侧 AI 视觉流水线：**MIPI 采集 → ISP 3A → RGA 预处理 → NPU 推理 → 画框显示 → MPP 编码 → RTSP 推流**。

在 RK3568（K1Mini）+ OV5647 + MIPI DSI 上验证通过，检测帧率 **15.8fps**。

## 特性

- **采集**：MIPI CSI → ISP21（AIQ 3A 自动曝光/白平衡）→ V4L2 **DMABUF 零拷贝**出图（1280×800@15fps）
- **AI 推理**：RGA 硬件预处理（NV12→RGB888 缩放 + letterbox，约 2.5ms）→ RKNN NPU 运行 YOLOv5 → 检测框回写
- **显示**：DRM KMS **原子接口（Atomic API）**非阻塞提交，驱动 MIPI DSI 竖屏（800×1280），RGA 硬件旋转 270°
- **推流**：RGA 缩放 → MPP H.264 硬编码 → GStreamer RTSP 服务器，VLC/ffplay 实时预览
- **零拷贝贯通**：相机 DMA → RGA → NPU → MPP 编码全链路共享 dma-buf，无 CPU 搬移
- **性能可观测**：全链路 perf 埋点，逐阶段统计耗时与帧率

## 架构

```
┌────────────────────────── RK3568 ───────────────────────────┐
│                                                              │
│  OV5647 (MIPI CSI)                                            │
│    │  RAW                                                        │
│    ▼                                                          │
│  ISP21 + AIQ 3A (AE/AWB)  ──▶  mainpath NV12 1280×800 (dma-fd)│
│    │                                                          │
│    ├───────────────────────▶ RGA 缩放+格式转换 (NV12→RGB888 640×640)
│    │                              │                           │
│    │                              ▼                           │
│    │                        RKNN NPU (YOLOv5)                 │
│    │                              │ 检测框                     │
│    │                              ▼                           │
│    │                        画框回写 (DMA_BUF_SYNC)            │
│    │                              │                           │
│    │                              ▼                           │
│    │                        RGA 旋转 270° → DRM KMS atomic    │
│    │                              │        │                  │
│    │                              │        └─▶ MIPI DSI 屏    │
│    │                              │                           │
│    └───────────────────────▶ MPP H.264 硬编码 → GStreamer RTSP │
│                                              │                 │
└──────────────────────────────────────────────┼─────────────────┘
                                               ▼
                                     rtsp://<device_ip>:8554/live
```

## 性能数据（100 帧统计）

| 阶段 | 优化前 | 优化后 | 手段 |
|---|---|---|---|
| 检测帧率 | 0.2fps（64 垃圾框） | **15.8fps** | 修复后处理双重 sigmoid 解码错误 |
| RKNN 推理 | 80.96ms | **45.52ms** | NPU 提频 200→900MHz |
| RGA 预处理 | 2.78ms | 2.50ms | letterbox 灰边懒填充 |
| VOP 显示 | 17.01ms | 3.43ms | DRM atomic 非阻塞提交（SetPlane 10.42ms→0.24ms） |
| MPP H264 编码 | 10.50ms | 3.20ms | 零拷贝输入 + 配置优化 |
| 推流流总耗时 | 15.42ms | 8.25ms | — |

> 注：NPU 提频到 900MHz 后推理耗时进一步降至 45.52ms，但帧率已达源帧率上限，无法继续提升；
> 900MHz 下瓶颈已从算力转向 DDR 带宽。

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

1. **修复检测失效**：0.2fps / 64 垃圾框 → 根因是后处理对 YOLOv5 输出重复 sigmoid，对照
   `rknn_model_zoo` 源码定位修复。
2. **预处理提速**：CPU letterbox 23ms → RGA 硬件 2.5ms。
3. **显示提速 80%**：`drmModeSetPlane`（同步阻塞 ~10.4ms）→ DRM atomic 非阻塞提交（0.24ms）。
4. **NPU 提频**：devfreq 强制 900MHz，推理 80.96→45.52ms。
5. **零拷贝贯通**：采集→RGA→NPU→编码全链路 dma-buf，无 CPU 搬移。

## 已知限制

- 依赖 Rockchip SDK 与闭源库，**不能脱离 SDK 独立构建**（见 `dependencies.md`）。
- 仅验证 RK3568 + OV5647 + Ubuntu 18.04 rootfs（glibc 2.27）。
- 模型不随仓库分发（见 `model/README.md`）。

## License

- 本项目 Apache-2.0（见 [LICENSE](LICENSE)）。
- 各组件许可与归属见 [NOTICE.md](NOTICE.md)。
- 提交前请阅读 [CONTRIBUTING.md](CONTRIBUTING.md)。
