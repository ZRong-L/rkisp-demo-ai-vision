/* SPDX-License-Identifier: Apache-2.0
 * rkaiq_rtsp.h — 内嵌 GStreamer RTSP 服务器
 *
 * 将 MPP 编码后的 H264 数据通过 appsrc 喂给 GStreamer pipeline，
 * 经 rtph264pay 封装后由内嵌 RTSP 服务器发送。
 *
 * VLC 拉流: rtsp://<device_ip>:8554/live
 */

#ifndef RKAIQ_RTSP_H
#define RKAIQ_RTSP_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 启动 RTSP 服务器（内部启动 GMainLoop 线程） */
int rkaiq_rtsp_start(int port);

/* 推送一帧 H264 数据到 RTSP 流 */
int rkaiq_rtsp_push_frame(const uint8_t *h264_data, size_t len, int64_t pts_us);

/* 停止 RTSP 服务器 */
void rkaiq_rtsp_stop(void);

#ifdef __cplusplus
}
#endif

#endif /* RKAIQ_RTSP_H */
