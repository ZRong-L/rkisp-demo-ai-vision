/* SPDX-License-Identifier: Apache-2.0
 * rkaiq_rtsp.c — 内嵌 GStreamer RTSP 服务器
 * VLC: rtsp://<device_ip>:8554/live
 *
 * ============================================================
 *  GStreamer RTSP 服务器原理
 * ============================================================
 *  传统 RTSP 服务（如 VLC 播放流）需要：
 *     1. 一个媒体源（摄像头/文件）产生数据
 *     2. 一个编码器把数据压成 H264
 *     3. 一个 RTSP 服务器把 H264 打包成 RTP 并通过网络发送
 *
 *  本文件用 GStreamer 的 rtsp-server 库实现了第 3 步：
 *  - 媒体源(H264 数据) 来自外部：isp_demo 的 MPP 编码器
 *  - 我们用 appsrc 这个 GStreamer 元素"喂"数据进去
 *  - GStreamer 负责把数据走 pipeline → RTP 打包 → RTSP 发送
 *
 *  关键 GStreamer 对象：
 *     GstRTSPServer     — RTSP 服务器（监听 8554 端口）
 *     GstRTSPMountPoints — URL 挂载表（把 "/live" 映射到某个媒体）
 *     GstRTSPMediaFactory — 媒体工厂（客户端请求时，按 launch 字符串创建 pipeline）
 *     GstRTSPMedia       — 一次客户端会话对应的 pipeline
 *     GstElement appsrc   — 应用数据源（我们 push 数据进去的地方）
 *     GMainLoop           — GLib 主事件循环（驱动 GStreamer 所有回调）
 * ============================================================ */
#include "rkaiq_rtsp.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server-object.h>
#include <gst/rtsp-server/rtsp-mount-points.h>
#include <gst/rtsp-server/rtsp-media-factory.h>
#include <gst/rtsp-server/rtsp-media.h>
#include <pthread.h>

/* 全局状态：服务器句柄、主循环、appsrc 数据源、时间基准 */
static struct {
    GstRTSPServer  *server;   /* RTSP 服务器对象（监听端口、响应客户端） */
    GMainLoop      *loop;     /* GLib 主循环（GStreamer 事件驱动核心） */
    GstElement     *appsrc;   /* 应用数据源元素（isp_demo 往这里推 H264） */
    GstClockTime    base_time;/* 时间基准（PTS 从 0 开始计算） */
    int started;              /* 服务器是否已启动 */
} s = {0};

/* 保护 s.appsrc 的互斥锁：media_configure_cb（GStreamer 线程）写，
 * rkaiq_rtsp_push_frame / rkaiq_rtsp_stop（isp_demo 主线程）读。
 * 防止多客户端滚动连断时出现数据竞争和内存泄漏。 */
static pthread_mutex_t rtsp_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * media_configure_cb — 媒体配置完成回调
 *
 * 在"每个客户端连接并创建 pipeline 之后"触发此回调。
 * 这是获取 appsrc 元素的时机：
 *   - 客户端 VLC 请求 rtsp://.../live 时，服务器按 factory 的 launch 字符串
 *     创建一条 pipeline（appsrc → h264parse → rtph264pay）
 *   - pipeline 创建完成后触发本回调
 *   - 我们从 pipeline 里取出名为 "src" 的元素（就是 appsrc），保存到全局 s.appsrc
 *   - 之后 isp_demo 的 rkaiq_rtsp_push_frame() 就往 s.appsrc 里推 H264 数据
 *
 * 参数：
 *   factory — 媒体工厂（我们创建的，这里没用）
 *   media   — 本次会话的媒体对象，包含实际创建的 pipeline
 *   udata   — 用户数据（g_signal_connect 时传入的，这里没用）
 */
static void media_configure_cb(GstRTSPMediaFactory *factory,
                                GstRTSPMedia *media, gpointer udata)
{
    (void)factory; (void)udata;
    /* 从 media 对象中取出 pipeline（一条 GstBin 容器） */
    GstElement *pipeline = gst_rtsp_media_get_element(media);
    if (!pipeline) {
        return;
    }

    /* 从 pipeline 中按名字 "src" 查找 appsrc 元素 */
    GstElement *new_appsrc = gst_bin_get_by_name(GST_BIN(pipeline), "src");
    if (!new_appsrc) {
        gst_object_unref(pipeline);
        return;
    }

    /* 加锁替换全局 appsrc：先释放旧引用（防泄漏），再写入新指针 */
    pthread_mutex_lock(&rtsp_lock);
    if (s.appsrc) {
        gst_object_unref(s.appsrc);
    }
    s.appsrc = new_appsrc;
    pthread_mutex_unlock(&rtsp_lock);

    /*
     * 配置 appsrc 为"实时流"模式：
     *   format=time       —— appsrc 按时间戳(time)驱动数据流，而不是按 byte/segment
     *   is-live=TRUE      —— 声明数据是实时的，不等待缓冲（低延迟）
     *   do-timestamp=TRUE —— 若推入的 buffer 没有 PTS，由 appsrc 自动打时间戳
     */
    gst_util_set_object_arg(G_OBJECT(s.appsrc), "format", "time");
    g_object_set(G_OBJECT(s.appsrc), "is-live", TRUE, "do-timestamp", TRUE, NULL);

    /* 打印 pipeline 的 dot 拓扑图到环境变量 GST_DEBUG_DUMP_DOT_DIR 指定的目录 */
    GST_DEBUG_BIN_TO_DOT_FILE_WITH_TS(GST_BIN(pipeline), GST_STATE_PLAYING, "rtsp_pipeline");

    gst_object_unref(pipeline);

    printf("[rtsp] media ready\n");
    /* 通知编码器：已有客户端连接，开始编码推流 */
    rkaiq_encoder_set_client(1);
}

/*
 * server_thread — RTSP 服务器主线程
 *
 * 所有 GStreamer RTSP 服务器逻辑都跑在这个独立线程里，
 * 不能阻塞 isp_demo 的主线程（采帧 + 推理）。
 */
static gpointer server_thread(gpointer data)
{
    (void)data;
    /* 初始化 GStreamer 库（必须在任何其他 GStreamer 调用之前调用） */
    gst_init(NULL, NULL);

    /* 1. 创建 RTSP 服务器对象 */
    s.server = gst_rtsp_server_new();
    /* 配置监听地址和端口：0.0.0.0 表示所有网卡，8554 是 RTSP 默认端口 */
    g_object_set(s.server, "service", "8554", "address", "0.0.0.0", NULL);

    /* 2. 获取 URL 挂载表（管理 rtsp://ip:8554/xxx 的路径映射） */
    GstRTSPMountPoints *mounts = gst_rtsp_server_get_mount_points(s.server);

    /*
     * 3. 创建媒体工厂
     *    "工厂"的作用：当客户端连接时，按 launch 字符串动态创建 pipeline。
     *    每次连接都会新建一条 pipeline（因为 set_shared 只共享 buffer 不共享 pipeline）。
     */
    GstRTSPMediaFactory *factory = gst_rtsp_media_factory_new();

    /*
     * 4. 设置 launch 字符串（这是 GStreamer 的 pipeline 描述语法）
     *    下面这行拆开解释：
     *      appsrc name=src                     —— 数据源：appsrc 元素，命名 "src"
     *      video/x-h264,stream-format=byte-stream,alignment=nal,framerate=30/1
     *                                          —— Caps 过滤：声明输入是 H264
     *                                               byte-stream = Annex-B 格式（00 00 00 01 起始码）
     *                                               alignment=nal = 数据按 NAL 单元对齐
     *                                               framerate=30/1 = 30 fps
     *      h264parse config-interval=-1        —— H264 解析器；config-interval=-1 表示
     *                                               在关键帧前强制插入 SPS/PPS 头，
     *                                               这样 VLC 随时加入都能解码
     *      rtph264pay name=pay0 pt=96          —— RTP 打包器：把 H264 NAL 封装成 RTP 包
     *                                               pt=96 = RTP payload type 96（H264 常用）
     */
    gst_rtsp_media_factory_set_launch(factory,
        "( appsrc name=src ! "
        "video/x-h264,stream-format=byte-stream,alignment=nal,framerate=30/1 ! "
        "h264parse config-interval=-1 ! "
        "rtph264pay name=pay0 pt=96 )");

    /* 共享媒体：多个客户端可同时观看同一路流 */
    gst_rtsp_media_factory_set_shared(factory, TRUE);

    /* 连接 "media-configure" 信号 → pipeline 创建后回调 media_configure_cb，
     * 我们在这里拿到 appsrc */
    g_signal_connect(factory, "media-configure",
                     G_CALLBACK(media_configure_cb), NULL);

    /* 5. 把工厂挂到 URL 路径 "/live" 上 → 客户端访问 rtsp://ip:8554/live */
    gst_rtsp_mount_points_add_factory(mounts, "/live", factory);
    g_object_unref(mounts);

    /* 6. 把服务器附加到 GLib 默认主上下文，开始监听 8554 端口 */
    gst_rtsp_server_attach(s.server, NULL);
    printf("[rtsp] rtsp://0.0.0.0:8554/live\n");

    /*
     * 7. 创建并运行 GLib 主事件循环
     *    GStreamer 的回调（media-configure）、网络 IO、客户端管理都由这个循环驱动。
     *    g_main_loop_run() 会阻塞直到 g_main_loop_quit() 被调用。
     */
    s.loop = g_main_loop_new(NULL, FALSE);
    g_main_loop_run(s.loop);

    /* 8. 退出清理 */
    pthread_mutex_lock(&rtsp_lock);
    if (s.appsrc) {
        gst_object_unref(s.appsrc);
    }
    s.appsrc = NULL;
    pthread_mutex_unlock(&rtsp_lock);
    g_main_loop_unref(s.loop);
    gst_object_unref(s.server);
    s.loop = NULL;
    s.server = NULL;
    return NULL;
}

/*
 * rkaiq_rtsp_start — 启动 RTSP 服务器
 *
 * 在独立线程中运行 server_thread()，避免阻塞主线程。
 * port 参数目前忽略（端口硬编码在 launch 字符串所在处 g_object_set 为 8554）。
 *
 * 返回 0 成功，-1 失败。
 */
int rkaiq_rtsp_start(int port)
{
    if (s.started) {
        return 0;
    }
    (void)port;

    /* 创建并启动独立线程，函数名为 "rtsp" */
    GThread *t = g_thread_new("rtsp", server_thread, NULL);
    if (!t) {
        return -1;
    }
    g_thread_unref(t);  /* 线程创建后释放引用（线程仍在运行） */

    /* 短暂等待 500ms，确保服务器初始化完成再返回 */
    g_usleep(500000);
    s.started = 1;
    printf("[rtsp] started\n");
    return 0;
}

/*
 * rkaiq_rtsp_push_frame — 推送一帧 H264 数据到 RTSP 服务器
 *
 * 由 isp_demo 的 MPP 编码器调用，把编码好的 H264 码流喂给 GStreamer appsrc。
 *
 * 参数：
 *   data — H264 码流数据（byte-stream 格式）
 *   len  — 数据长度（字节）
 *   pts  — 显示时间戳（当前忽略，用系统时钟自动生成）
 *
 * 返回 0 成功，-1 失败（服务器未启动 / 无客户端 / 推送被拒绝）。
 */
int rkaiq_rtsp_push_frame(const uint8_t *data, size_t len, int64_t pts)
{
    (void)pts;
    if (!s.started || len == 0) {
        return -1;
    }

    /* 加锁取得 appsrc 的有效引用，防止并发更新导致悬空指针；
     * 推流过程在锁外执行，不影响并发性能 */
    GstElement *appsrc;
    pthread_mutex_lock(&rtsp_lock);
    appsrc = s.appsrc;
    if (appsrc) {
        gst_object_ref(appsrc);
    }
    pthread_mutex_unlock(&rtsp_lock);
    if (!appsrc) {
        return -1;
    }

    /* 1. 创建 GStreamer buffer，并拷贝 H264 数据进去
     *    gst_buffer_new_allocate(NULL, len, NULL) — 分配 len 字节的 buffer */
    GstBuffer *buf = gst_buffer_new_allocate(NULL, len, NULL);
    gst_buffer_fill(buf, 0, data, len);

    /*
     * 2. 计算时间戳（PTS）
     *    用单调时钟(system clock)生成时间戳：
     *       now = 当前单调时钟值
     *       base_time = 第一次推流时的时钟值（基准）
     *       PTS = now - base_time  → 从 0 开始的相对时间
     *    设置 PTS/DTS/DURATION，让 GStreamer 知道每帧的播放时间和时长。
     */
    GstClock *clk = gst_system_clock_obtain();
    if (clk) {
        GstClockTime now = gst_clock_get_time(clk);
        gst_object_unref(clk);
        if (!s.base_time) {
            s.base_time = now;   /* 记录基准时间 */
        }
        GST_BUFFER_PTS(buf) = now - s.base_time;                    /* 显示时间 */
        GST_BUFFER_DTS(buf) = GST_BUFFER_PTS(buf);                  /* 解码时间（同 PTS） */
        GST_BUFFER_DURATION(buf) = gst_util_uint64_scale_int(1, GST_SECOND, 30); /* 1/30 秒 = 33ms */
    }

    /*
     * 3. 把 buffer 推给 appsrc
     *    g_signal_emit_by_name(s.appsrc, "push-buffer", buf, &ret)
     *      —— 相当于调用 appsrc 元素的 "push-buffer" 动作
     *      —— GStreamer 内部会把这个 buffer 送入 pipeline 继续处理
     *         （h264parse 解析 → rtph264pay 打包 RTP → 发送给客户端）
     *      —— ret 保存返回值（GstFlowReturn）
     */
    GstFlowReturn ret;
    g_signal_emit_by_name(appsrc, "push-buffer", buf, &ret);
    gst_buffer_unref(buf);

    if (ret != GST_FLOW_OK) {
        fprintf(stderr, "[rtsp] push err: %s\n", gst_flow_get_name(ret));
        return -1;
    }
    return 0;
}

/*
 * rkaiq_rtsp_stop — 停止 RTSP 服务器
 *
 * 向 appsrc 发送 end-of-stream 信号，通知客户端流结束；
 * 然后退出主循环并清理资源。
 */
void rkaiq_rtsp_stop(void)
{
    if (!s.started) {
        return;
    }
    /* 通知 GStreamer 数据流结束（加锁取引用，防止并发竞争） */
    GstElement *appsrc = NULL;
    pthread_mutex_lock(&rtsp_lock);
    appsrc = s.appsrc;
    if (appsrc) {
        gst_object_ref(appsrc);
    }
    pthread_mutex_unlock(&rtsp_lock);
    if (appsrc) {
        g_signal_emit_by_name(appsrc, "end-of-stream", NULL);
        gst_object_unref(appsrc);
    }
    /* 退出主循环（server_thread 中的 g_main_loop_run 会返回） */
    if (s.loop) {
        g_main_loop_quit(s.loop);
    }
    /* 等 200ms 让清理完成 */
    g_usleep(200000);
    memset(&s, 0, sizeof(s));
    printf("[rtsp] stopped\n");
}
