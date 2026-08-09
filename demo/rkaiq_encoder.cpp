/* SPDX-License-Identifier: Apache-2.0
 *
 * rkaiq_encoder.cpp — 编码器线程
 *
 * ── 数据流 ──
 *
 * 主线程                    编码线程
 * ──────                    ──────
 * DQBUF
 * YOLO + 画框
 * List_Push_With_Cond(job)
 *   │                        List_Pop_With_Cond() ← 阻塞直到有 job
 *   │                        RGA scale(camera_fd)
 *   │                        condition_post(rga_done)
 * condition_wait(rga_done) ←
 * DRM显示 → QBUF
 *                            [有客户端?]
 *                              MPP encode → RTSP push
 *                            [无客户端?]
 *                              跳过，继续等下一帧
 *
 * cam_list 内部自带 mutex + condvar，
 * condition 用于单次完成通知（done flag 模式）。
 */

#include "rkaiq_encoder.h"
#include "rkaiq_rtsp.h"
#include "rkaiq_perf.h"
#include "cam_list.h"
#include "condition.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

#include "rga.h"
#include "RgaApi.h"
#include "rk_mpi.h"
#include "mpp_enc_cfg.h"
#include "mpp_buffer.h"
#include "rk_venc_rc.h"

#define ENC_W   640
#define ENC_H   480
#define ENC_FPS 30
#define ENC_BPS (2 * 1024 * 1024)

/* ───────────────────── 线程间传递的帧信息 ───────────────────── */
typedef struct {
    int camera_fd;  /* V4L2 DMABUF fd，编码线程做 RGA 缩放时读取 */
} FrameJob;

static struct {
    /* RGA */
    int    rga_fd;
    void  *rga_addr;
    bo_t   rga_bo;

    /* MPP 编码器 */
    MppCtx         mpp_ctx;
    MppApi        *mpp_api;
    MppBufferGroup buf_grp;
    uint64_t       frame_count;
    uint8_t       *hdr_data;   /* SPS/PPS */
    size_t         hdr_len;

    /* 线程 */
    pthread_t       tid;
    struct condition rga_done;  /* 主线程在此等待 RGA 完成 */
    LIST_HANDLE     queue;      /* FrameJob 队列 */

    int client;       /* RTSP 客户端在线标志 */
    int idr_request;
    int quit;
    int isp_w;        /* ISP 实际输出宽度 (可能被对齐为 608 等) */
    int isp_h;        /* ISP 实际输出高度 */

    int initialized;
} s = {0};
#ifdef DEBUG_USE_TIME
static perf_counter_t g_perf_enc_rga   = { .name = "编码流 RGA NV12->NV12" };
static perf_counter_t g_perf_enc_mpp   = { .name = "MPP H264e" };
static perf_counter_t g_perf_enc_rtsp  = { .name = "RTSP 推送" };
static perf_counter_t g_perf_enc_total = { .name = "推流流总耗时" };
#endif

/* ───────────────────── RGA 缩放 ───────────────────── */
static int rga_scale(int camera_fd) {
    rga_info_t src, dst;

    memset(&src, 0, sizeof(src));
    src.fd = camera_fd;
    src.mmuFlag = 1;
    rga_set_rect(&src.rect, 0, 0, s.isp_w, s.isp_h, s.isp_w, s.isp_h,
                 RK_FORMAT_YCbCr_420_SP);
    memset(&dst, 0, sizeof(dst));
    dst.fd = s.rga_fd;
    dst.mmuFlag = 1;
    rga_set_rect(&dst.rect, 0, 0, ENC_W, ENC_H, ENC_W, ENC_H,
                 RK_FORMAT_YCbCr_420_SP);

    return c_RkRgaBlit(&src, &dst, NULL);
}

/* ───────────────────── 编码线程主循环 ───────────────────── */
static void* encoder_loop(void *arg) {
    (void)arg;

    while (!s.quit) {
        /* 阻塞等待主线程提交帧 */
        FrameJob *job = (FrameJob*)List_Pop_With_Cond(s.queue);
        if (!job)
            continue;

        /* 1. 无 RTSP 客户端 → 不编码不推流，直接等下一帧 */
        if (!s.client) {
            free(job);
            condition_post(&s.rga_done);            /* 通知主线程可以安全 QBUF */
            continue;
        }
        int fd = job->camera_fd;

        /* 2. RGA 硬件缩放：从 camera DMABUF 读到本线程 RGA buffer */
#ifdef DEBUG_USE_TIME
        uint64_t t1 = get_timestamp();   /* T1: 编码流 RGA NV12->NV12 开始 */
#endif
        rga_scale(fd);
// #ifdef DEBUG_USE_TIME
//         uint64_t t2 = get_timestamp();   /* T2: RGA 结束 = MPP 开始 */
// #endif

        /* 3. RGA 完成，通知主线程可以安全 QBUF */
        condition_post(&s.rga_done);

        free(job);

        /* 4. 有客户端 → MPP 硬件编码 + RTSP 推送 */

        if (s.idr_request) {
            s.mpp_api->control(s.mpp_ctx, MPP_ENC_SET_IDR_FRAME, NULL);
            s.idr_request = 0;
        }

        MppFrame frame = NULL;
        mpp_frame_init(&frame);
        mpp_frame_set_width(frame, ENC_W);
        mpp_frame_set_height(frame, ENC_H);
        mpp_frame_set_hor_stride(frame, ENC_W);
        mpp_frame_set_ver_stride(frame, ENC_H);
        mpp_frame_set_fmt(frame, MPP_FMT_YUV420SP);
        mpp_frame_set_pts(frame, s.frame_count * (1000000 / ENC_FPS));

        MppBuffer mpp_buf = NULL;
        {
            static int import_log_once = 0;
            MppBufferInfo info;

            /* 零拷贝：直接导入 RGA 输出 dma-buf，省去 memcpy 到 MPP buffer。
             * info.type 必须匹配 fd 的实际类型（ION 或 DMA_HEAP），不能为 NORMAL(0) */
            memset(&info, 0, sizeof(info));
            info.size = ENC_W * ENC_H * 3 / 2;
            info.fd = s.rga_fd;

            /* 用 ION 类型导入（MPP 对任意 dma-buf fd 兼容，实测正常）。
             * 注意：DMA_HEAP 类型会把 fd 当作 dma-heap 分配的内存，
             * 但 RGA 的 fd 实际由 librga 内部分配，类型不匹配会导致系统卡死。 */
            info.type = MPP_BUFFER_TYPE_ION;
            if (mpp_buffer_import(&mpp_buf, &info) != MPP_OK || !mpp_buf) {
                /* 导入失败：回退到 mpp_buffer_get + memcpy 方式 */
                if (mpp_buf) {
                    mpp_buffer_put(mpp_buf);
                    mpp_buf = NULL;
                }
                int sz = ENC_W * ENC_H * 3 / 2;
                mpp_buffer_get(s.buf_grp, &mpp_buf, sz);
                if (mpp_buf)
                    memcpy(mpp_buffer_get_ptr(mpp_buf), s.rga_addr, sz);
                if (!import_log_once) {
                    printf("[encoder] MPP zero-copy ION import failed, fallback to memcpy\n");
                    import_log_once = 1;
                }
            } else if (!import_log_once) {
                printf("[encoder] MPP zero-copy: ION import OK\n");
                import_log_once = 1;
            }
            mpp_frame_set_buffer(frame, mpp_buf);
        }

        if (s.mpp_api->encode_put_frame(s.mpp_ctx, frame) == MPP_OK) {
            MppPacket pkt = NULL;
            if (s.mpp_api->encode_get_packet(s.mpp_ctx, &pkt) == MPP_OK && pkt) {
                size_t len = mpp_packet_get_length(pkt);
                if (len > 0) {
// #ifdef DEBUG_USE_TIME
//                     uint64_t t3 = get_timestamp();   /* T3: MPP H264e 结束 = RTSP 开始 */
// #endif
                    /* 每帧前附上 SPS/PPS，VLC 随时加入都能解 */
                    if (s.hdr_data && s.hdr_len)
                        rkaiq_rtsp_push_frame(s.hdr_data, s.hdr_len, 0);

                    rkaiq_rtsp_push_frame(
                        (const uint8_t*)mpp_packet_get_data(pkt), len, 0);
#ifdef DEBUG_USE_TIME
                    uint64_t t4 = get_timestamp();   /* T4: RTSP Server 推送完成 */
                    // perf_sample(&g_perf_enc_rga,   (double)(t2 - t1));  /* 编码流 RGA 耗时 */
                    // perf_sample(&g_perf_enc_mpp,   (double)(t3 - t2));  /* MPP H264e 耗时 */
                    // perf_sample(&g_perf_enc_rtsp,  (double)(t4 - t3));  /* RTSP 推送耗时 */
                    perf_sample(&g_perf_enc_total, (double)(t4 - t1));  /* 推流流总耗时+帧率 */
#endif
                }
                mpp_packet_deinit(&pkt);
            }
        }
        mpp_frame_deinit(&frame);
        if (mpp_buf)
            mpp_buffer_put(mpp_buf);

        if (s.mpp_api && s.frame_count > 0 && (s.frame_count % 100) == 0)
            printf("[encoder] encoded %d frames\n", s.frame_count);

        s.frame_count++;
    }
    return NULL;
}

int rkaiq_encoder_init(int isp_w, int isp_h) {
    if (s.initialized)
        return 0;

    /* RGA */
    memset(&s.rga_bo, 0, sizeof(s.rga_bo));
    if (c_RkRgaGetAllocBuffer(&s.rga_bo, ENC_W, ENC_H, 12))
        return -1;
    if (c_RkRgaGetMmap(&s.rga_bo))
        return -1;
    if (c_RkRgaGetBufferFd(&s.rga_bo, &s.rga_fd))
        return -1;
    s.rga_addr = s.rga_bo.ptr;
    printf("[rga] finish init\n");

    /* MPP */
    {
        int ret;
        MppEncCfg cfg = NULL;

        ret = mpp_create(&s.mpp_ctx, &s.mpp_api);
        if (ret) return -1;

        ret = mpp_init(s.mpp_ctx, MPP_CTX_ENC, MPP_VIDEO_CodingAVC);
        if (ret) return -1;

        mpp_enc_cfg_init(&cfg);
        mpp_enc_cfg_set_s32(cfg, "prep:width",      ENC_W);
        mpp_enc_cfg_set_s32(cfg, "prep:height",     ENC_H);
        mpp_enc_cfg_set_s32(cfg, "prep:hor_stride",  ENC_W);
        mpp_enc_cfg_set_s32(cfg, "prep:ver_stride",  ENC_H);
        mpp_enc_cfg_set_s32(cfg, "prep:format",      MPP_FMT_YUV420SP);
        mpp_enc_cfg_set_s32(cfg, "rc:mode",          MPP_ENC_RC_MODE_CBR);
        mpp_enc_cfg_set_s32(cfg, "rc:bps_target",    ENC_BPS);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_in_num",    ENC_FPS);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_in_denorm", 1);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_out_num",   ENC_FPS);
        mpp_enc_cfg_set_s32(cfg, "rc:fps_out_denorm",1);

        ret = s.mpp_api->control(s.mpp_ctx, MPP_ENC_SET_CFG, cfg);
        mpp_enc_cfg_deinit(cfg);
        if (ret) return -1;

        mpp_buffer_group_get_internal(&s.buf_grp, MPP_BUFFER_TYPE_ION);

        /* 获取并保存 SPS/PPS 头（每帧推送用） */
        MppPacket hdr = NULL;
        MppBuffer hdr_buf = NULL;
        mpp_buffer_get(s.buf_grp, &hdr_buf, 128*1024);
        mpp_packet_init_with_buffer(&hdr, hdr_buf);
        mpp_packet_set_length(hdr, 0);

        if (s.mpp_api->control(s.mpp_ctx, MPP_ENC_GET_HDR_SYNC, hdr) == MPP_OK) {

            size_t hl = mpp_packet_get_length(hdr);
            s.hdr_data = (uint8_t*)malloc(hl);
            if (s.hdr_data) {
                memcpy(s.hdr_data, mpp_packet_get_data(hdr), hl);
                s.hdr_len = hl;

            }
        }
        mpp_packet_deinit(&hdr);
    if (hdr_buf) mpp_buffer_put(hdr_buf);
    }
    printf("[mpp] finish init\n");

    /* 线程同步 */
    condition_init(&s.rga_done);
    s.queue = List_Create(true);
    s.client = 0;
    s.idr_request = 0;
    s.quit = 0;
    s.isp_w = isp_w;
    s.isp_h = isp_h;

    pthread_create(&s.tid, NULL, encoder_loop, NULL);

    rkaiq_rtsp_start(8554);
    s.initialized = 1;
    printf("[encoder] thread running\n");
    return 0;
}
void rkaiq_encoder_submit(int camera_fd) {
    FrameJob *job = (FrameJob*)malloc(sizeof(FrameJob));
    if (!job) return;
    job->camera_fd = camera_fd;
    List_Push_With_Cond(s.queue, job);
}

void rkaiq_encoder_wait_rga(void) {
    condition_wait(&s.rga_done);
}

void rkaiq_encoder_set_client(int connected) {
    s.client = connected;
    if (connected)
        s.idr_request = 1;
}

int  rkaiq_encoder_has_client(void) { return s.client; }
void rkaiq_encoder_request_idr(void) { s.idr_request = 1; }
void rkaiq_encoder_deinit(void) {
    if (!s.initialized)
        return;

    s.quit = 1;

    /* 送入一个空 job 唤醒编码线程使其退出 */
    FrameJob *wake = (FrameJob*)malloc(sizeof(FrameJob));
    if (wake) {
        wake->camera_fd = -1;
        List_Push_With_Cond(s.queue, wake);
    }
    pthread_join(s.tid, NULL);

    free(s.hdr_data);
    mpp_destroy(s.mpp_ctx);
    c_RkRgaUnmap(&s.rga_bo);
    c_RkRgaFree(&s.rga_bo);
    if (s.buf_grp)
        mpp_buffer_group_put(s.buf_grp);

    List_Destroy(s.queue);
    condition_deinit(&s.rga_done);

    memset(&s, 0, sizeof(s));
    printf("[encoder] deinit\n");
}
