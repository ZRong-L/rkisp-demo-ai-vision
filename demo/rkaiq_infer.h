/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Part of the RK3568 end-side AI vision pipeline (rkisp-demo extension).
 * 在 Rockchip rkisp_demo 基础上扩展的多媒体 AI 流水线模块。
 */
/*
 * rkaiq_infer.h — RKISP Demo YOLOv5 推理 API
 *
 * 基于官方 rknn_model_zoo postprocess.cc，复用 rkaiq_preprocess 做 RGA 缩放。
 */

#ifndef RKAIQ_INFER_H
#define RKAIQ_INFER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 检测结果 */
typedef struct {
    int left;
    int top;
    int right;
    int bottom;
    float confidence;
    char name[16];
} rkaiq_detect_result_t;

typedef struct {
    int count;
    rkaiq_detect_result_t results[128];
} rkaiq_detect_result_group_t;

/* API */
int rkaiq_infer_init(const char *model_path);
int rkaiq_infer_process(int camera_fd, void *camera_addr,
                        int cam_w, int cam_h,
                        rkaiq_detect_result_group_t *results);
void rkaiq_infer_deinit(void);
int rkaiq_infer_is_ready(void);
void rkaiq_draw_boxes_nv12(uint8_t *nv12, int stride, int w, int h,
                           const rkaiq_detect_result_group_t *results);

#ifdef __cplusplus
}
#endif

#endif /* RKAIQ_INFER_H */
