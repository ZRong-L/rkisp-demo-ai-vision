# NOTICE

本仓库混合了多个来源的代码与二进制，各自适用不同的许可。请按以下清单逐一核对。

## 各组件归属与许可

| 组件 | 来源 | 许可 | 说明 |
|---|---|---|---|
| `demo/` 下 Rockchip 原版文件（`display.c`、`rkdrm_display.c`、`rkisp_demo.cpp` 主体、`ae/af/awb_algo_demo`、`iio`、`sample` 等） | Rockchip `camera_engine_rkaiq/rkisp_demo` | GPL **或** OpenIB.org BSD（原文件头声明，**本项目按 BSD 选项再分发**） | 保留原文件头版权声明，未修改其许可 |
| `demo/postprocess.cc`、`demo/postprocess_sdk.cc` | Rockchip `rknn_model_zoo` | Apache-2.0（文件头已声明） | 保留原版权头 |
| `demo/` 下本项目新增模块（`rkaiq_*`、`drmDsp.*`、`rkRgaApi.*`、`cam_list.*`、`yolov5.h` 等） | 本项目 | Apache-2.0 | 文件头均带 `SPDX-License-Identifier: Apache-2.0` |
| `demo/libs/include/gstreamer-1.0/gst/rtsp-server/*.h`（25 个） | gst-rtsp-server 1.16.3 | **LGPL-2.1** | 随仓库提供以支持编译；头文件保留原版权头 |
| `libgstrtspserver-1.0.so*` | gst-rtsp-server（LGPL）或 SDK 拷贝 | **LGPL-2.1** | **不随仓库分发**，动态链接，见 `scripts/fetch-libs.sh` |
| `demo/libs/arm64/*.so`（`librknnrt.so`、`librga.so`、`libdrm.so`、`librockchip_mpp.so`） | Rockchip SDK | **闭源** | **不随仓库分发**，由 `scripts/fetch-libs.sh` 从用户本地 SDK 拷贝 |
| YOLOv5 `.rknn` 模型 | 由 Ultralytics YOLOv5 权重转换而来 | 见 `model/README.md`（**不随仓库分发**） | 训练代码 AGPL-3.0，请自行评估模型再分发风险 |

## 再分发声明

- 仓库中**不包含**任何 Rockchip 闭源二进制、模型文件。
- 构建前必须运行 `./scripts/fetch-libs.sh <SDK_ROOT>` 从本地 SDK 补齐闭源库。
- 使用 LGPL 库时请遵守 LGPL-2.1 要求（动态链接、保留版权声明、提供重链接方式）。

## 商标

本项目与 Rockchip、GStreamer、Ultralytics 无隶属关系。相关商标归各自所有者所有。
