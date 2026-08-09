/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Part of the RK3568 end-side AI vision pipeline (rkisp-demo extension).
 * 在 Rockchip rkisp_demo 基础上扩展的多媒体 AI 流水线模块。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/dma-buf.h>

#include "RgaApi.h"
#include "drmrga.h"
#include "rga.h"
#include "rkaiq_preprocess.h"
#include "rkaiq_perf.h"
#include "rkRgaApi.h"

/* 模型输入 buffer (640x640 RGB888, 供 RKNN 模型输入) —— RGA dma-buf */
static int  g_model_fd = -1;
static void *g_model_addr = NULL;
static bo_t g_model_bo;

/* letterbox 参数：保持宽高比缩放后的缩放比例和灰边偏移 */
static float g_letterbox_scale = 1.0f;
static int   g_offset_x = 0;
static int   g_offset_y = 0;

/* 灰边懒填充状态：letterbox 参数由相机分辨率决定，只在首次（或分辨率变化）时填一次灰边，
 * 之后 RGA 每帧只写内容区（dst rect 不会覆盖灰边）。 */
static int s_gray_done = 0;
static int s_gray_cw = 0, s_gray_ch = 0, s_gray_ox = 0, s_gray_oy = 0;

int preprocess_init(void)
{
    int ret;

    ret = c_RkRgaInit();
    if (ret) {
        printf("[preprocess] RGA init failed: %d\n", ret);
        return -1;
    }

    /* 分配 640x640 RGB888 dma-buf (RGA 目标) */
    memset(&g_model_bo, 0, sizeof(g_model_bo));
    ret = c_RkRgaGetAllocBuffer(&g_model_bo, MODEL_INPUT_WIDTH, MODEL_INPUT_HEIGHT, 24);
    if (ret) {
        printf("[preprocess] alloc buffer failed: %d\n", ret);
        return -1;
    }
    ret = c_RkRgaGetMmap(&g_model_bo);
    if (ret) {
        printf("[preprocess] mmap failed: %d\n", ret);
        c_RkRgaFree(&g_model_bo);
        return -1;
    }
    ret = c_RkRgaGetBufferFd(&g_model_bo, &g_model_fd);
    if (ret) {
        printf("[preprocess] get fd failed: %d\n", ret);
        c_RkRgaUnmap(&g_model_bo);
        c_RkRgaFree(&g_model_bo);
        return -1;
    }
    g_model_addr = g_model_bo.ptr;
    printf("[preprocess] model buf: %dx%d RGB888, fd=%d, addr=%p\n",
           MODEL_INPUT_WIDTH, MODEL_INPUT_HEIGHT, g_model_fd, g_model_addr);
    return 0;
}

int preprocess_scale(int camera_dma_fd, void *src_addr, int src_w, int src_h)
{
    (void)src_addr;   /* RGA 经 fd 读相机（DMA），CPU 不直接读 */
    if (g_model_fd < 0 || !g_model_addr) {
        printf("[preprocess] not initialized\n");
        return -1;
    }

    /* letterbox：保持宽高比缩放，剩余区域填灰(114) */
    float scale = (float)MODEL_INPUT_WIDTH / src_w;
    float scale_h = (float)MODEL_INPUT_HEIGHT / src_h;
    if (scale_h < scale) {
        scale = scale_h;
    }
    int content_w = (int)(src_w * scale);
    int content_h = (int)(src_h * scale);
    int offset_x = (MODEL_INPUT_WIDTH  - content_w) / 2;
    int offset_y = (MODEL_INPUT_HEIGHT - content_h) / 2;
    g_letterbox_scale = scale;
    g_offset_x = offset_x;
    g_offset_y = offset_y;

    /* RGA 硬件缩放 + NV12->RGB888（硬件加速，实测 ~2.8ms vs CPU letterbox ~23ms）。
     * RGA 的 dst rect 只写内容区（offset_x/y 定位），不覆盖灰边，所以每帧只做这一次。 */
    int ret = rkRgaBlitEx(camera_dma_fd, src_w, src_h, RK_FORMAT_YCbCr_420_SP,
                          g_model_fd, content_w, content_h, RK_FORMAT_RGB_888,
                          offset_x, offset_y, MODEL_INPUT_WIDTH, MODEL_INPUT_HEIGHT);
    if (ret) {
        printf("[preprocess] scale failed: %d\n", ret);
        return ret;
    }

    /* 灰边懒填充：只在首次（或相机分辨率变化导致 letterbox 参数改变）时填一次 + 刷回物理，
     * 之后每帧直接复用，省掉每帧 memset + SYNC_WRITE（含 1.2MB cache flush）。 */
    if (!s_gray_done || s_gray_cw != content_w || s_gray_ch != content_h ||
        s_gray_ox != offset_x || s_gray_oy != offset_y) {
        uint8_t *p = (uint8_t *)g_model_addr;
        int row_bytes = MODEL_INPUT_WIDTH * 3;
        memset(p, 114, (size_t)offset_y * row_bytes);
        memset(p + (size_t)(offset_y + content_h) * row_bytes, 114,
               (size_t)(MODEL_INPUT_HEIGHT - offset_y - content_h) * row_bytes);
        for (int row = offset_y; row < offset_y + content_h; row++) {
            uint8_t *r = p + (size_t)row * row_bytes;
            memset(r, 114, (size_t)offset_x * 3);
            memset(r + (size_t)(offset_x + content_w) * 3, 114,
                   (size_t)(MODEL_INPUT_WIDTH - offset_x - content_w) * 3);
        }
        /* CPU 写的灰边刷回物理，供 NPU DMA 读（零拷贝） */
        struct dma_buf_sync sync = { 0 };
        sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_WRITE;
        ioctl(g_model_fd, DMA_BUF_IOCTL_SYNC, &sync);
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_WRITE;
        ioctl(g_model_fd, DMA_BUF_IOCTL_SYNC, &sync);

        s_gray_done = 1;
        s_gray_cw = content_w;
        s_gray_ch = content_h;
        s_gray_ox = offset_x;
        s_gray_oy = offset_y;
    }

    return 0;
}

float preprocess_get_letterbox_scale(void) { return g_letterbox_scale; }
int   preprocess_get_offset_x(void)        { return g_offset_x; }
int   preprocess_get_offset_y(void)        { return g_offset_y; }
int   preprocess_get_dma_fd(void)          { return g_model_fd; }
void* preprocess_get_vaddr(void)           { return g_model_addr; }

void preprocess_deinit(void)
{
    if (g_model_fd >= 0)
        close(g_model_fd);
    if (g_model_bo.ptr) {
        c_RkRgaUnmap(&g_model_bo);
        c_RkRgaFree(&g_model_bo);
    }
    g_model_fd = -1;
    g_model_addr = NULL;
    memset(&g_model_bo, 0, sizeof(g_model_bo));
    printf("[preprocess] deinit done\n");
}
