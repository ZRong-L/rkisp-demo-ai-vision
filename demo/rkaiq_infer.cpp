/* SPDX-License-Identifier: Apache-2.0
 * rkaiq_infer.cpp — RKISP Demo YOLOv5 推理
 * RGA缩放→heap buf→RKNN→SDK post_process
 * 支持 NV12 画框 + 8x13 点阵字体标签
 */
#include "rkaiq_infer.h"
#include "rkaiq_preprocess.h"
#include "rkaiq_perf.h"
#include "postprocess_sdk.h"
#include "rknn_api.h"
#include "RgaApi.h"
#include "drmrga.h"
#include "rga.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/ioctl.h>
#include <linux/dma-buf.h>

/* 零拷贝(DMABUF)输入：1 = 启用。
 * 之前零拷贝实测 64 垃圾框，是与 post_process 双重 sigmoid 缺陷混淆（该缺陷对任何喂法都出垃圾）。
 * post_process 修复后重测零拷贝 pass_through=0 + UINT8（NPU 文档 4.4.5.2/表4-1 配置），
 * 若正常则保留 DMABUF（省去 memcpy，冲 15fps）。 */
#define USE_ZERO_COPY 1

static struct {
    rknn_context ctx;
    int model_w, model_h, model_ch;
    rknn_input_output_num io_num;
    int32_t *qnt_zp;
    float *qnt_scale;
    rknn_tensor_attr input_attr;   /* 零拷贝输入属性 */
    rknn_tensor_mem *input_mem;    /* 零拷贝输入内存（来自 preprocess dma-buf） */
    /* temporal smoothing */
    rkaiq_detect_result_t last_results[128];
    int last_count;
    int hold_frames;
} s = {0};

#ifdef DEBUG_USE_TIME
rkaiq_perf_ai_t g_perf_ai = {0};
static perf_counter_t g_perf_rga  = { .name = "AI RGA NV12->RGB" };
static perf_counter_t g_perf_rknn = { .name = "AI RKNN YOLOv5" };
#endif

int rkaiq_infer_init(const char *path)
{
    if (s.ctx) {
        return 0;
    }

    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return -1;
    }

    fseek(fp, 0, SEEK_END);
    long len = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    char *mdl = (char *)malloc(len);
    if (!mdl || (long)fread(mdl, 1, len, fp) != len) {
        free(mdl);
        fclose(fp);
        return -1;
    }
    fclose(fp);

    /* 实验B：去掉 RKNN_FLAG_COLLECT_PERF_MASK（该标志让每帧推理额外收集逐层数据，拖慢推理）。
       如需重做逐层耗时/fallback 分析：改回 RKNN_FLAG_COLLECT_PERF_MASK 并取消下方 #if 0 块 */
    int ret = rknn_init(&s.ctx, mdl, len, 0, NULL);
    free(mdl);
    if (ret != RKNN_SUCC) {
        printf("rknn_init:%d\n", ret);
        return -1;
    }

    rknn_query(s.ctx, RKNN_QUERY_IN_OUT_NUM, &s.io_num, sizeof(s.io_num));
    printf("rkaiq_infer:%din,%dout\n", s.io_num.n_input, s.io_num.n_output);

    rknn_tensor_attr ia;
    memset(&ia, 0, sizeof(ia));
    ia.index = 0;
    rknn_query(s.ctx, RKNN_QUERY_INPUT_ATTR, &ia, sizeof(ia));
    if (ia.fmt == RKNN_TENSOR_NCHW) {
        s.model_ch = ia.dims[1];
        s.model_h = ia.dims[2];
        s.model_w = ia.dims[3];
    } else {
        s.model_h = ia.dims[1];
        s.model_w = ia.dims[2];
        s.model_ch = ia.dims[3];
    }

#if USE_ZERO_COPY
    /* 零拷贝输入：从 preprocess 的 dma-buf fd 创建 RKNN 输入内存，省去每帧 memcpy。
     * pass_through=0 + type=UINT8（NPU 文档 4.4.5.2/表4-1：RK3568 零拷贝输入配置与通用 API 一致，
     * int8 模型可设 uint8 输入，runtime 负责归一化+量化）。 */
    s.input_attr = ia;
    s.input_attr.pass_through = 0;
    s.input_attr.type = RKNN_TENSOR_UINT8;
    s.input_mem = rknn_create_mem_from_fd(s.ctx,
                                          preprocess_get_dma_fd(),
                                          preprocess_get_vaddr(),
                                          s.model_w * s.model_h * s.model_ch, 0);
    if (!s.input_mem) {
        printf("rknn_create_mem_from_fd failed\n");
        return -1;
    }
    if (rknn_set_io_mem(s.ctx, s.input_mem, &s.input_attr) != RKNN_SUCC) {
        printf("rknn_set_io_mem failed\n");
        return -1;
    }
#endif

    int n_out = (s.io_num.n_output < 3) ? s.io_num.n_output : 3;
    s.qnt_zp = new int32_t[n_out];
    s.qnt_scale = new float[n_out];
    for (int i = 0; i < n_out; i++) {
        rknn_tensor_attr oa;
        memset(&oa, 0, sizeof(oa));
        oa.index = i;
        rknn_query(s.ctx, RKNN_QUERY_OUTPUT_ATTR, &oa, sizeof(oa));
        s.qnt_zp[i] = oa.zp;
        s.qnt_scale[i] = oa.scale;
    }

    printf("rkaiq_infer:%dx%dch=%d (zero-copy input)\n", s.model_w, s.model_h, s.model_ch);
    return 0;
}

void rkaiq_infer_deinit(void)
{
    if (!s.ctx) {
        return;
    }
    if (s.input_mem) {
        rknn_destroy_mem(s.ctx, s.input_mem);
        s.input_mem = NULL;
    }
    rknn_destroy(s.ctx);
    delete[] s.qnt_zp;
    delete[] s.qnt_scale;
    memset(&s, 0, sizeof(s));
}

int rkaiq_infer_is_ready(void)
{
    return s.ctx != 0;
}

int rkaiq_infer_process(int fd, void *addr, int cw, int ch,
                        rkaiq_detect_result_group_t *r)
{
    if (!s.ctx) {
        return -1;
    }

#ifdef DEBUG_USE_TIME
    uint64_t _t1 = get_timestamp();   /* T1: CPU letterbox NV12->RGB 开始 */
#endif
    if (preprocess_scale(fd, addr, cw, ch)) {
        return -1;
    }
// #ifdef DEBUG_USE_TIME
//     uint64_t _t2 = get_timestamp();   /* T2: CPU letterbox 结束 = RKNN 开始 */
// #endif
#if USE_ZERO_COPY
    /* 零拷贝：NPU 直接读取 preprocess dma-buf（init 已绑定 input_mem），省去 memcpy。
     * preprocess 已在 CPU letterbox 后 SYNC_WRITE 刷回，NPU 经 fd 直接读物理内存。 */
    if (rknn_run(s.ctx, NULL) != 0) {
        return -1;
    }
#else
    /* Test A2 + 时间戳：定位 CPU 烧在哪个阶段（pre=RGA预处理 in=memcpy+inputs_set run=推理） */
    {
        static uint8_t *cpu_buf = NULL;
        static int cpu_buf_size = 0;
        int need = s.model_w * s.model_h * s.model_ch;
        if (!cpu_buf || cpu_buf_size < need) {
            free(cpu_buf);
            cpu_buf = (uint8_t *)malloc(need);
            cpu_buf_size = need;
        }
        uint64_t t_in_a = get_timestamp();
        /* SYNC_READ：保证 CPU 能看到 RGA 写入的内容 */
        struct dma_buf_sync rds = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
        ioctl(preprocess_get_dma_fd(), DMA_BUF_IOCTL_SYNC, &rds);
        memcpy(cpu_buf, preprocess_get_vaddr(), need);
        struct dma_buf_sync rde = { .flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ };
        ioctl(preprocess_get_dma_fd(), DMA_BUF_IOCTL_SYNC, &rde);

        rknn_input in;
        memset(&in, 0, sizeof(in));
        in.index = 0;
        in.type = RKNN_TENSOR_UINT8;
        in.size = need;
        in.fmt = RKNN_TENSOR_NHWC;
        in.buf = cpu_buf;
        if (rknn_inputs_set(s.ctx, 1, &in) != 0) {
            return -1;
        }
        uint64_t t_in_b = get_timestamp();
        if (rknn_run(s.ctx, NULL) != 0) {
            return -1;
        }
        uint64_t t_in_c = get_timestamp();
        /* 时间戳 + 输入均值诊断，每 2 帧打印 */
        static int dbg_t = 0;
        if ((dbg_t++ % 2) == 0) {
            long long sum = 0;
            for (int i = 0; i < need; i++) sum += cpu_buf[i];
            printf("[T] pre=%ldus in=%ldus run=%ldus mean=%.1f\n",
                   (long)(_t2 - _t1), (long)(t_in_b - t_in_a), (long)(t_in_c - t_in_b),
                   (double)sum / need);
        }
    }
#endif

#if 0  /* 逐层耗时已禁用(实验B)：COLLECT_PERF_MASK 关闭后此查询无数据且本身有开销 */
    static int perf_detail_cnt = 0;
    if (++perf_detail_cnt % 10 == 1) {   /* 每 10 帧打印一次，避免刷屏 */
        rknn_perf_detail perf_detail;
        memset(&perf_detail, 0, sizeof(perf_detail));
        rknn_query(s.ctx, RKNN_QUERY_PERF_DETAIL, &perf_detail, sizeof(perf_detail));
        printf("==== RKNN Perf Detail ====\n%s\n", perf_detail.perf_data);
    }
#endif

    int n_out = (s.io_num.n_output < 3) ? s.io_num.n_output : 3;
    rknn_output out[3];
    memset(out, 0, sizeof(out));
    for (int i = 0; i < n_out; i++) {
        out[i].index = i;
        out[i].want_float = 0;
    }
    if (rknn_outputs_get(s.ctx, n_out, out, NULL) != 0) {
        return -1;
    }

    /*
     * letterbox：post_process 的 scale 用均匀缩放比例，
     * 返回后减掉灰边偏移，把模型坐标映射回相机坐标。
     *   SDK: box = model_coord / scale
     *   相机坐标 = (model_coord - offset) / scale
     */
    float lb_scale = preprocess_get_letterbox_scale();
    int off_x = preprocess_get_offset_x();
    int off_y = preprocess_get_offset_y();
    std::vector<int32_t> zp;
    std::vector<float> sc;
    for (int i = 0; i < n_out; i++) {
        zp.push_back(s.qnt_zp[i]);
        sc.push_back(s.qnt_scale[i]);
    }

    detect_result_group_t od;
    memset(&od, 0, sizeof(od));
    post_process((int8_t *)out[0].buf, (int8_t *)out[1].buf, (int8_t *)out[2].buf,
                 s.model_h, s.model_w, 0.15f, 0.45f, lb_scale, lb_scale, zp, sc, &od);
    rknn_outputs_release(s.ctx, n_out, out);

    /* 逆映射：减掉灰边偏移，映射回相机坐标 */
    for (int i = 0; i < od.count; i++) {
        od.results[i].box.left   -= (int)(off_x / lb_scale);
        od.results[i].box.right  -= (int)(off_x / lb_scale);
        od.results[i].box.top    -= (int)(off_y / lb_scale);
        od.results[i].box.bottom -= (int)(off_y / lb_scale);
    }

    /* 按最终置信度过滤：低于阈值(0.1)的框直接丢弃，减少误检（排查剪刀时临时从 0.25 降到 0.1） */
    if (od.count > 0) {
        int valid = 0;
        for (int i = 0; i < od.count; i++) {
            if (od.results[i].prop >= 0.1f) {
                od.results[valid++] = od.results[i];
            }
        }
        od.count = valid;
    }
#ifdef DEBUG_USE_TIME
    // uint64_t _t3 = get_timestamp();   /* T3: RKNN 结束 = 画框回写 开始 */
    g_perf_ai.ai_t1 = _t1;
    // g_perf_ai.ai_t2 = _t2;
    // g_perf_ai.ai_t3 = _t3;
    // perf_sample(&g_perf_rga,  (double)(_t2 - _t1));   /* AI RGA 耗时 */
    // perf_sample(&g_perf_rknn, (double)(_t3 - _t2));   /* AI RKNN 耗时(含输出获取+后处理) */
#endif

    if (od.count > 0) {
        printf("YOLO: %d object(s) detected\n", od.count);
        for (int i = 0; i < od.count; i++) {
            printf("  [%d] %-12s conf=%.1f%% box=(%d,%d)-(%d,%d)\n",
                   i, od.results[i].name,
                   od.results[i].prop * 100.0f,
                   od.results[i].box.left, od.results[i].box.top,
                   od.results[i].box.right, od.results[i].box.bottom);
        }
    }

    /* temporal smoothing: keep last detections for 3 frames */
    if (od.count > 0) {
        s.last_count = (od.count > 128) ? 128 : od.count;
        for (int i = 0; i < s.last_count; i++) {
            s.last_results[i].left = od.results[i].box.left;
            s.last_results[i].top = od.results[i].box.top;
            s.last_results[i].right = od.results[i].box.right;
            s.last_results[i].bottom = od.results[i].box.bottom;
            s.last_results[i].confidence = od.results[i].prop;
            strncpy(s.last_results[i].name, od.results[i].name, 15);
            s.last_results[i].name[15] = 0;
        }
        s.hold_frames = 3;
    } else if (s.hold_frames > 0) {
        s.hold_frames--;
    }

    r->count = (s.hold_frames > 0) ? s.last_count : 0;
    for (int i = 0; i < r->count; i++) {
        r->results[i] = s.last_results[i];
    }
    return r->count;
}

/* ============================================================
 *  8x13 bitmap font + NV12 drawing
 * ============================================================ */
#include <stdint.h>

/* VGA 8x13 font for ASCII 32-126, 13 bytes/char */
static const uint8_t font8x13[95][13] = {
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /*   */
    {0x00,0x10,0x10,0x10,0x10,0x10,0x10,0x10,0x00,0x10,0x10,0x00,0x00}, /* ! */
    {0x00,0x24,0x24,0x24,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* " */
    {0x00,0x24,0x24,0x7E,0x24,0x24,0x7E,0x24,0x24,0x00,0x00,0x00,0x00}, /* # */
    {0x00,0x08,0x3E,0x49,0x48,0x3E,0x09,0x49,0x3E,0x08,0x00,0x00,0x00}, /* $ */
    {0x00,0x60,0x92,0x94,0x68,0x08,0x16,0x29,0x49,0x06,0x00,0x00,0x00}, /* % */
    {0x00,0x18,0x24,0x24,0x18,0x28,0x46,0x44,0x3A,0x00,0x00,0x00,0x00}, /* & */
    {0x00,0x10,0x10,0x10,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ' */
    {0x00,0x04,0x08,0x08,0x10,0x10,0x10,0x08,0x08,0x04,0x00,0x00,0x00}, /* ( */
    {0x00,0x20,0x10,0x10,0x08,0x08,0x08,0x10,0x10,0x20,0x00,0x00,0x00}, /* ) */
    {0x00,0x00,0x24,0x18,0x7E,0x18,0x24,0x00,0x00,0x00,0x00,0x00,0x00}, /* * */
    {0x00,0x00,0x08,0x08,0x3E,0x08,0x08,0x00,0x00,0x00,0x00,0x00,0x00}, /* + */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x10,0x20,0x00,0x00}, /* , */
    {0x00,0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* - */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x10,0x10,0x00,0x00,0x00}, /* . */
    {0x00,0x02,0x04,0x04,0x08,0x08,0x10,0x10,0x20,0x20,0x00,0x00,0x00}, /* / */
    {0x00,0x3C,0x42,0x46,0x4A,0x52,0x62,0x42,0x3C,0x00,0x00,0x00,0x00}, /* 0 */
    {0x00,0x08,0x18,0x28,0x08,0x08,0x08,0x08,0x3E,0x00,0x00,0x00,0x00}, /* 1 */
    {0x00,0x3C,0x42,0x02,0x0C,0x30,0x40,0x40,0x7E,0x00,0x00,0x00,0x00}, /* 2 */
    {0x00,0x3C,0x42,0x02,0x1C,0x02,0x02,0x42,0x3C,0x00,0x00,0x00,0x00}, /* 3 */
    {0x00,0x04,0x0C,0x14,0x24,0x44,0x7E,0x04,0x04,0x00,0x00,0x00,0x00}, /* 4 */
    {0x00,0x7E,0x40,0x40,0x7C,0x02,0x02,0x42,0x3C,0x00,0x00,0x00,0x00}, /* 5 */
    {0x00,0x1C,0x20,0x40,0x7C,0x42,0x42,0x42,0x3C,0x00,0x00,0x00,0x00}, /* 6 */
    {0x00,0x7E,0x02,0x04,0x08,0x10,0x10,0x10,0x10,0x00,0x00,0x00,0x00}, /* 7 */
    {0x00,0x3C,0x42,0x42,0x3C,0x42,0x42,0x42,0x3C,0x00,0x00,0x00,0x00}, /* 8 */
    {0x00,0x3C,0x42,0x42,0x42,0x3E,0x02,0x04,0x38,0x00,0x00,0x00,0x00}, /* 9 */
    {0x00,0x00,0x00,0x10,0x10,0x00,0x00,0x10,0x10,0x00,0x00,0x00,0x00}, /* : */
    {0x00,0x00,0x00,0x10,0x10,0x00,0x00,0x10,0x10,0x20,0x00,0x00,0x00}, /* ; */
    {0x00,0x00,0x04,0x08,0x10,0x20,0x10,0x08,0x04,0x00,0x00,0x00,0x00}, /* < */
    {0x00,0x00,0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00,0x00,0x00,0x00}, /* = */
    {0x00,0x00,0x20,0x10,0x08,0x04,0x08,0x10,0x20,0x00,0x00,0x00,0x00}, /* > */
    {0x00,0x3C,0x42,0x02,0x0C,0x10,0x10,0x00,0x10,0x00,0x00,0x00,0x00}, /* ? */
    {0x00,0x3C,0x4A,0x56,0x5E,0x5C,0x40,0x42,0x3C,0x00,0x00,0x00,0x00}, /* @ */
    {0x00,0x18,0x24,0x42,0x42,0x7E,0x42,0x42,0x42,0x00,0x00,0x00,0x00}, /* A */
    {0x00,0x7C,0x42,0x42,0x7C,0x42,0x42,0x42,0x7C,0x00,0x00,0x00,0x00}, /* B */
    {0x00,0x3C,0x42,0x40,0x40,0x40,0x40,0x42,0x3C,0x00,0x00,0x00,0x00}, /* C */
    {0x00,0x78,0x44,0x42,0x42,0x42,0x42,0x44,0x78,0x00,0x00,0x00,0x00}, /* D */
    {0x00,0x7E,0x40,0x40,0x7C,0x40,0x40,0x40,0x7E,0x00,0x00,0x00,0x00}, /* E */
    {0x00,0x7E,0x40,0x40,0x7C,0x40,0x40,0x40,0x40,0x00,0x00,0x00,0x00}, /* F */
    {0x00,0x3C,0x42,0x40,0x40,0x4E,0x42,0x42,0x3C,0x00,0x00,0x00,0x00}, /* G */
    {0x00,0x42,0x42,0x42,0x7E,0x42,0x42,0x42,0x42,0x00,0x00,0x00,0x00}, /* H */
    {0x00,0x3E,0x08,0x08,0x08,0x08,0x08,0x08,0x3E,0x00,0x00,0x00,0x00}, /* I */
    {0x00,0x02,0x02,0x02,0x02,0x02,0x02,0x42,0x3C,0x00,0x00,0x00,0x00}, /* J */
    {0x00,0x42,0x44,0x48,0x50,0x60,0x50,0x48,0x44,0x00,0x00,0x00,0x00}, /* K */
    {0x00,0x40,0x40,0x40,0x40,0x40,0x40,0x40,0x7E,0x00,0x00,0x00,0x00}, /* L */
    {0x00,0x42,0x66,0x5A,0x5A,0x42,0x42,0x42,0x42,0x00,0x00,0x00,0x00}, /* M */
    {0x00,0x42,0x62,0x52,0x4A,0x46,0x42,0x42,0x42,0x00,0x00,0x00,0x00}, /* N */
    {0x00,0x3C,0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00,0x00,0x00,0x00}, /* O */
    {0x00,0x7C,0x42,0x42,0x7C,0x40,0x40,0x40,0x40,0x00,0x00,0x00,0x00}, /* P */
    {0x00,0x3C,0x42,0x42,0x42,0x42,0x4A,0x44,0x3A,0x00,0x00,0x00,0x00}, /* Q */
    {0x00,0x7C,0x42,0x42,0x7C,0x48,0x44,0x42,0x42,0x00,0x00,0x00,0x00}, /* R */
    {0x00,0x3C,0x42,0x40,0x3C,0x02,0x02,0x42,0x3C,0x00,0x00,0x00,0x00}, /* S */
    {0x00,0x7F,0x08,0x08,0x08,0x08,0x08,0x08,0x08,0x00,0x00,0x00,0x00}, /* T */
    {0x00,0x42,0x42,0x42,0x42,0x42,0x42,0x42,0x3C,0x00,0x00,0x00,0x00}, /* U */
    {0x00,0x42,0x42,0x42,0x42,0x42,0x24,0x18,0x00,0x00,0x00,0x00,0x00}, /* V */
    {0x00,0x42,0x42,0x42,0x5A,0x5A,0x66,0x42,0x00,0x00,0x00,0x00,0x00}, /* W */
    {0x00,0x42,0x42,0x24,0x18,0x18,0x24,0x42,0x42,0x00,0x00,0x00,0x00}, /* X */
    {0x00,0x41,0x22,0x14,0x08,0x08,0x08,0x08,0x08,0x00,0x00,0x00,0x00}, /* Y */
    {0x00,0x7E,0x02,0x04,0x08,0x10,0x20,0x40,0x7E,0x00,0x00,0x00,0x00}, /* Z */
    {0x00,0x0E,0x08,0x08,0x08,0x08,0x08,0x08,0x0E,0x00,0x00,0x00,0x00}, /* [ */
    {0x00,0x20,0x10,0x10,0x08,0x08,0x04,0x04,0x02,0x02,0x00,0x00,0x00}, /* \ */
    {0x00,0x70,0x10,0x10,0x10,0x10,0x10,0x10,0x70,0x00,0x00,0x00,0x00}, /* ] */
    {0x00,0x08,0x14,0x22,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ^ */
    {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x7E,0x00,0x00}, /* _ */
    {0x00,0x10,0x08,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00}, /* ` */
};

/* Draw a single char at (x,y) on NV12. char c in [32,126] */
static void draw_char_nv12(uint8_t *nv, int st, int w, int h, int x, int y, char c)
{
    if (c < 32 || c > 126) {
        return;
    }
    int ci = c - 32;
    for (int row = 0; row < 13; row++) {
        if (y + row >= h) {
            break;
        }
        uint8_t bits = font8x13[ci][row];
        for (int col = 0; col < 8; col++) {
            if (x + col >= w) {
                break;
            }
            if (!(bits & (1 << (7 - col)))) {
                continue;
            }
            int px = x + col, py = y + row;
            nv[py * st + px] = 255;  /* white Y */
            /* UV */
            if (py % 2 == 0 && px % 2 == 0) {
                int ux = px / 2, uy = py / 2;
                nv[st * h + (uy * (st / 2) + ux) * 2] = 128;
                nv[st * h + (uy * (st / 2) + ux) * 2 + 1] = 128;
            }
        }
    }
}

/* Draw text on NV12 at (x,y) */
static void draw_text_nv12(uint8_t *nv, int st, int w, int h, int x, int y, const char *t)
{
    while (*t) {
        draw_char_nv12(nv, st, w, h, x, y, *t);
        x += 9;
        t++;
    }
}

void rkaiq_draw_boxes_nv12(uint8_t *nv, int st, int w, int h,
                           const rkaiq_detect_result_group_t *r)
{
    char lbl[32];
    for (int i = 0; i < r->count; i++) {
        int x1 = r->results[i].left, y1 = r->results[i].top;
        int x2 = r->results[i].right, y2 = r->results[i].bottom;
        if (x1 < 0) {
            x1 = 0;
        }
        if (y1 < 0) {
            y1 = 0;
        }
        if (x2 >= w) {
            x2 = w - 1;
        }
        if (y2 >= h) {
            y2 = h - 1;
        }
        if (x2 <= x1 || y2 <= y1) {
            continue;
        }

        /* Green box (Y=130, UV=90,40) - 2px border */
        for (int t = 0; t < 2; t++) {
            int yy = y1 + t;
            if (yy < h) {
                memset(&nv[yy * st + x1], 130, x2 - x1 + 1);
            }
            yy = y2 - t;
            if (yy >= 0) {
                memset(&nv[yy * st + x1], 130, x2 - x1 + 1);
            }
        }
        for (int y = y1; y <= y2; y++) {
            if (x1 < w) {
                nv[y * st + x1] = 130;
            }
            if (x1 + 1 < w) {
                nv[y * st + x1 + 1] = 130;
            }
            if (x2 >= 0) {
                nv[y * st + x2] = 130;
            }
            if (x2 - 1 >= 0) {
                nv[y * st + x2 - 1] = 130;
            }
        }
        uint8_t *uv = nv + st * h;
        int uv2 = st / 2;
        for (int t = 0; t < 2; t++) {
            int yy = y1 + t;
            if (yy >= h) {
                continue;
            }
            for (int x = x1; x <= x2; x += 2) {
                uv[(yy / 2 * uv2 + x / 2) * 2] = 90;
                uv[(yy / 2 * uv2 + x / 2) * 2 + 1] = 40;
            }
            yy = y2 - t;
            if (yy < 0) {
                continue;
            }
            for (int x = x1; x <= x2; x += 2) {
                uv[(yy / 2 * uv2 + x / 2) * 2] = 90;
                uv[(yy / 2 * uv2 + x / 2) * 2 + 1] = 40;
            }
        }
        for (int y = y1 + 2; y < y2 - 1; y += 2) {
            for (int t = 0; t < 2; t++) {
                int xx = x1 + t;
                if (xx >= w) {
                    continue;
                }
                uv[(y / 2 * uv2 + xx / 2) * 2] = 90;
                uv[(y / 2 * uv2 + xx / 2) * 2 + 1] = 40;
                xx = x2 - t;
                if (xx < 0) {
                    continue;
                }
                uv[(y / 2 * uv2 + xx / 2) * 2] = 90;
                uv[(y / 2 * uv2 + xx / 2) * 2 + 1] = 40;
            }
        }

        /* Draw label background + text inside bar above box */
        snprintf(lbl, sizeof(lbl), "%s %.0f%%", r->results[i].name, r->results[i].confidence * 100.0f);
        for (char *p = lbl; *p; p++) {
            if (*p >= 'a' && *p <= 'z') {
                *p = *p - 'a' + 'A';
            }
        }
        int tw = strlen(lbl) * 9, tx = x1, ty2 = y1 - 2; /* bar bottom = 2px above box top */
        if (ty2 - 13 < 0) {
            ty2 = y1 + 15; /* if too high, put below box top */
        }
        if (tx + tw > w) {
            tx = w - tw;
        }

        /* Draw gray bar (Y=180, UV=128gray) */
        for (int yy = ty2 - 13; yy < ty2; yy++) {
            if (yy >= 0 && yy < h) {
                memset(&nv[yy * st + tx], 180, tw > st ? st : tw);
            }
        }
        uint8_t *uv3 = nv + st * h;
        int uv3s = st / 2;
        for (int yy = ty2 - 13; yy < ty2; yy += 2) {
            if (yy >= 0 && yy < h) {
                for (int xx = tx; xx < tx + tw && xx < w; xx += 2) {
                    uv3[(yy / 2 * uv3s + xx / 2) * 2] = 128;
                    uv3[(yy / 2 * uv3s + xx / 2) * 2 + 1] = 128;
                }
            }
        }

        /* Draw text at TOP of bar (not below it) */
        draw_text_nv12(nv, st, w, h, tx, ty2 - 13, lbl);
    }
}
