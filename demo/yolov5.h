/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Part of the RK3568 end-side AI vision pipeline (rkisp-demo extension).
 * 在 Rockchip rkisp_demo 基础上扩展的多媒体 AI 流水线模块。
 */
#ifndef _RKISP_YOLOV5_H_
#define _RKISP_YOLOV5_H_

#include "rknn_api.h"
#include "common.h"

typedef struct {
    rknn_context        rknn_ctx;
    rknn_input_output_num io_num;
    rknn_tensor_attr   *input_attrs;
    rknn_tensor_attr   *output_attrs;
    int                 model_channel;
    int                 model_width;
    int                 model_height;
    bool                is_quant;
} rknn_app_context_t;

#include "postprocess.h"
#include "image_utils.h"

#endif
