/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Part of the RK3568 end-side AI vision pipeline (rkisp-demo extension).
 * 在 Rockchip rkisp_demo 基础上扩展的多媒体 AI 流水线模块。
 */
#ifndef _RGA_API_H_
#define _RGA_API_H_

#ifdef __cplusplus
extern "C" {
#endif

struct rkRgaCfg {
    int width;
    int height;
    int fmt;
    int fd;
    void *addr;
};

void rkRgaInit();
int rkRgaBlit(struct rkRgaCfg *src_cfg, struct rkRgaCfg *dst_cfg);
int rkRgaBlitEx(int src_fd, int src_w, int src_h, int src_fmt,
                int dst_fd, int dst_w, int dst_h, int dst_fmt,
                int dst_x, int dst_y, int dst_stride_w, int dst_stride_h);
int rkRgaBlitRaw(int src_fd, int src_w, int src_h, int fmt,
                 int dst_fd, int dst_w, int dst_h, int rotation);

#ifdef __cplusplus
};
#endif
#endif
