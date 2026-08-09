/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RKAIQ_ENCODER_H
#define RKAIQ_ENCODER_H

#ifdef __cplusplus
extern "C" {
#endif

int rkaiq_encoder_init(int isp_w, int isp_h);
void rkaiq_encoder_deinit(void);

/* 主线程：提交 camera_fd，唤醒编码线程做 RGA 缩放 */
void rkaiq_encoder_submit(int camera_fd);

/* 主线程：阻塞等待 RGA 完成，返回后可安全 QBUF */
void rkaiq_encoder_wait_rga(void);

/* 由 RTSP 模块调用 */
void rkaiq_encoder_set_client(int connected);
void rkaiq_encoder_request_idr(void);
int  rkaiq_encoder_has_client(void);

#ifdef __cplusplus
}
#endif

#endif /* RKAIQ_ENCODER_H */
