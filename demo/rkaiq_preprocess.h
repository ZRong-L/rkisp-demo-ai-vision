/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Part of the RK3568 end-side AI vision pipeline (rkisp-demo extension).
 * 在 Rockchip rkisp_demo 基础上扩展的多媒体 AI 流水线模块。
 */
#ifndef RKAIQ_PREPROCESS_H
#define RKAIQ_PREPROCESS_H

/*
 * RGA 图像预处理模块
 * Camera 1280x720 NV12 → 模型输入 640x640 RGB888 (缩放+颜色转换)
 * 提供 DMABUF fd 用于后续 RKNN 零拷贝推理
 */

#ifdef __cplusplus
extern "C" {
#endif

/* 模型输入尺寸 */
#define MODEL_INPUT_WIDTH  640
#define MODEL_INPUT_HEIGHT 640

/* 初始化: 分配模型输入 buffer */
int preprocess_init(void);

/* 缩放+格式转换: 相机 NV12 (src_addr, src_w x src_h) → 模型输入 buffer (640x640 RGB888)
 * CPU letterbox（model_zoo 方式）：RGA NV12->RGB888 转换在 librga 1.8.1 上写黑屏，
 * 改用 CPU 直读相机 mmap 地址做缩放+颜色转换。src_addr = 相机缓冲 CPU 地址。 */
int preprocess_scale(int camera_dma_fd, void *src_addr, int src_w, int src_h);

/* 获取模型输入 buffer 信息 */
int  preprocess_get_dma_fd(void);
void* preprocess_get_vaddr(void);

/* letterbox 参数（供 rkaiq_infer_process 做检测框坐标逆映射） */
float preprocess_get_letterbox_scale(void);
int   preprocess_get_offset_x(void);
int   preprocess_get_offset_y(void);

/* 释放资源 */
void preprocess_deinit(void);

#ifdef __cplusplus
}
#endif

#endif /* RKAIQ_PREPROCESS_H */
