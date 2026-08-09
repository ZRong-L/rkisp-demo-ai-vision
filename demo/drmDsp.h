/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Part of the RK3568 end-side AI vision pipeline (rkisp-demo extension).
 * 在 Rockchip rkisp_demo 基础上扩展的多媒体 AI 流水线模块。
 */
#ifndef __DRM_DSP_H__
#define __DRM_DSP_H__
#ifdef __cplusplus

extern "C"
{
#endif

#include <drm_fourcc.h>

int initDrmDsp();
int drmDspFrame(int srcWidth, int srcHeight, int dispWidth, int dispHeight,
		int dmaFd, void* dstAddr, int fmt);
void deInitDrmDsp();
#ifdef __cplusplus
}
#endif

#endif

