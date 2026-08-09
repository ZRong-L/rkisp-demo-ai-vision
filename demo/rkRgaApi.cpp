/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Part of the RK3568 end-side AI vision pipeline (rkisp-demo extension).
 * 在 Rockchip rkisp_demo 基础上扩展的多媒体 AI 流水线模块。
 */
#ifdef Android
#include <utils/Singleton.h>
#include <RockchipRga_Android.h>
#else
#include "RockchipRga.h"
#endif
#include "rkRgaApi.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef Android
static RockchipRga rkRga;
#endif

void rkRgaInit()
{
#ifndef Android
    rkRga.RkRgaInit();
#endif
}

int rkRgaBlit(struct rkRgaCfg *src_cfg, struct rkRgaCfg *dst_cfg)
{
    int ret = 0;

#ifdef Android
    android::RockchipRga& rkRga(android::RockchipRga::get());
#endif

    /********** rga_info_t Init **********/
    rga_info_t src;
    rga_info_t dst;

    memset(&src, 0, sizeof(rga_info_t));
    src.fd = src_cfg->fd;
    src.mmuFlag = 1;
    src.virAddr = src_cfg->addr;

    memset(&dst, 0, sizeof(rga_info_t));
    dst.fd = dst_cfg->fd;
    dst.mmuFlag = 1;
    dst.virAddr = dst_cfg->addr;

    /********** set the rect_info **********/
    rga_set_rect(&src.rect, 0, 0, src_cfg->width, src_cfg->height, src_cfg->width/*stride*/, src_cfg->height, src_cfg->fmt);
    rga_set_rect(&dst.rect, 0, 0, dst_cfg->width, dst_cfg->height, dst_cfg->width/*stride*/, dst_cfg->height, dst_cfg->fmt);

    /************ set the rga_mod ,rotation\composition\scale\copy .... **********/
#if 0
    src.rotation = 0;
    src.rotation = HAL_TRANSFORM_ROT_90;
    src.rotation = HAL_TRANSFORM_ROT_180;
    src.rotation = HAL_TRANSFORM_ROT_270;
    src.rotation = HAL_TRANSFORM_FLIP_V;
    src.rotation = HAL_TRANSFORM_FLIP_H;
#endif

    /********** call rga_Interface **********/
    ret = rkRga.RkRgaBlit(&src, &dst, NULL);
    if (ret) {
	    printf("rgaFillColor error : %s\n", strerror(errno));
    }

    return ret;
}


/* 支持不同源/目标格式 + 目标偏移，用于 letterbox 居中缩放 */
int rkRgaBlitEx(int src_fd, int src_w, int src_h, int src_fmt,
                int dst_fd, int dst_w, int dst_h, int dst_fmt,
                int dst_x, int dst_y, int dst_stride_w, int dst_stride_h)
{
    rga_info_t src, dst;
    int ret;

    memset(&src, 0, sizeof(rga_info_t));
    src.fd = src_fd;
    src.mmuFlag = 1;

    memset(&dst, 0, sizeof(rga_info_t));
    dst.fd = dst_fd;
    dst.mmuFlag = 1;

    rga_set_rect(&src.rect, 0, 0, src_w, src_h, src_w, src_h, src_fmt);
    rga_set_rect(&dst.rect, dst_x, dst_y, dst_w, dst_h,
                 dst_stride_w, dst_stride_h, dst_fmt);

    ret = rkRga.RkRgaBlit(&src, &dst, NULL);
    if (ret)
        printf("rkRgaBlitEx error: %d\n", ret);
    return ret;
}

/* RGA 旋转+缩放：显示端把 camera 1280x800 NV12 旋转 270° -> 800x1280 */
int rkRgaBlitRaw(int src_fd, int src_w, int src_h, int fmt,
                 int dst_fd, int dst_w, int dst_h, int rotation)
{
    rga_info_t src, dst;
    int ret;
    int out_w, out_h, x_off, y_off;

    memset(&src, 0, sizeof(rga_info_t));
    src.fd = src_fd;
    src.mmuFlag = 1;

    memset(&dst, 0, sizeof(rga_info_t));
    dst.fd = dst_fd;
    dst.mmuFlag = 1;

    /*
     * 计算旋转后的自然输出尺寸：
     *   90°/270° 旋转会交换宽高（1280x720 旋转 270 后 = 720x1280）
     *   若目标 BO 比自然尺寸大，居中输出（两侧留黑边），避免拉伸变形
     */
    if (rotation == 0x01 || rotation == 0x07) {  /* 90° 或 270° */
        out_w = src_h;
        out_h = src_w;
    } else {
        out_w = src_w;
        out_h = src_h;
    }

    x_off = (dst_w - out_w) / 2;
    y_off = (dst_h - out_h) / 2;
    if (x_off < 0) x_off = 0;
    if (y_off < 0) y_off = 0;

    rga_set_rect(&src.rect, 0, 0, src_w, src_h, src_w, src_h, fmt);
    /* wstride/hstride 用 BO 实际尺寸，active rect 用自然输出尺寸+居中偏移 */
    rga_set_rect(&dst.rect, x_off, y_off, out_w, out_h, dst_w, dst_h, fmt);

    src.rotation = rotation;

    ret = rkRga.RkRgaBlit(&src, &dst, NULL);
    if (ret)
        printf("rkRgaBlitRaw error: %d\n", ret);
    return ret;
}

#ifdef __cplusplus
};
#endif
