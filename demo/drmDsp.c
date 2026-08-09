/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Part of the RK3568 end-side AI vision pipeline (rkisp-demo extension).
 * 在 Rockchip rkisp_demo 基础上扩展的多媒体 AI 流水线模块。
 */
#include <stdio.h>
#include <linux/fb.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <drm_fourcc.h>
#include <string.h>

#include "./drmDsp/dev.h"
#include "./drmDsp/bo.h"
#include "./drmDsp/modeset.h"
#include "./include/xf86drm.h"
#include "./include/xf86drmMode.h"
#include "drmDsp.h"
#include <stdbool.h>
#include <sys/time.h>

#if ISPDEMO_ENABLE_RGA
#include "rkRgaApi.h"
#include "rga.h"
#endif

/* ==== Atomic 非阻塞提交：plane 属性 ID（init 时查询一次） ==== */
#if ISPDEMO_ATOMIC_NONBLOCK
static uint32_t s_atomic_prop_fb_id, s_atomic_prop_crtc_id;
static uint32_t s_atomic_prop_src_x, s_atomic_prop_src_y, s_atomic_prop_src_w, s_atomic_prop_src_h;
static uint32_t s_atomic_prop_dst_x, s_atomic_prop_dst_y, s_atomic_prop_dst_w, s_atomic_prop_dst_h;
#endif

struct drmDsp {
  struct fb_var_screeninfo vinfo;
  unsigned long screensize;
  char* fbp;
  struct sp_dev* dev;
  struct sp_plane** plane;
  struct sp_crtc* test_crtc;
  struct sp_plane* test_plane;
  int num_test_planes;
  struct sp_bo* bo[2];
  struct sp_bo* nextbo;
} gDrmDsp;


int initDrmDsp() {
  int ret = 0, i = 0;
  struct drmDsp* pDrmDsp = &gDrmDsp;

  memset(pDrmDsp, 0, sizeof(struct drmDsp));

  pDrmDsp->dev = create_sp_dev();
  if (!pDrmDsp->dev) {
    printf("Failed to create sp_dev\n");
    return -1;
  }

  ret = initialize_screens(pDrmDsp->dev);
  if (ret) {
    printf("Failed to initialize screens\n");
    return ret;
  }
#if ISPDEMO_ATOMIC_NONBLOCK
  /* 原子非阻塞提交必须先开启 DRM atomic client 能力，否则 atomic 调用返回 EINVAL */
  drmSetClientCap(pDrmDsp->dev->fd, DRM_CLIENT_CAP_ATOMIC, 1);
#endif
  pDrmDsp->plane = (struct sp_plane**)calloc(pDrmDsp->dev->num_planes, sizeof(struct sp_plane*));
  if (!pDrmDsp->plane) {
    printf("Failed to allocate plane array\n");
    return -1;
  }

  /* Find the active CRTC (one already initialized by initialize_screens) */
  pDrmDsp->test_crtc = NULL;
  for (i = 0; i < pDrmDsp->dev->num_crtcs; i++) {
    if (pDrmDsp->dev->crtcs[i].scanout) {
      pDrmDsp->test_crtc = &pDrmDsp->dev->crtcs[i];
      break;
    }
  }
  if (!pDrmDsp->test_crtc) {
    printf("No active CRTC found (is display connected?)\n");
    return -1;
  }

  pDrmDsp->num_test_planes = pDrmDsp->test_crtc->num_planes;
  for (i = 0; i < pDrmDsp->test_crtc->num_planes; i++) {
    pDrmDsp->plane[i] = get_sp_plane(pDrmDsp->dev, pDrmDsp->test_crtc);
    if (is_supported_format(pDrmDsp->plane[i], DRM_FORMAT_NV12)) {
      pDrmDsp->test_plane = pDrmDsp->plane[i];
      break;
    }
  }
  if (!pDrmDsp->test_plane) {
    printf("No NV12-capable plane found on active CRTC\n");
    return -1;
  }
#if 1
  pDrmDsp->num_test_planes = pDrmDsp->test_crtc->num_planes;
  pDrmDsp->test_plane = NULL;
  printf("\n--- Scanning all planes for CRTC %d ---\n", pDrmDsp->test_crtc->crtc->crtc_id);

  // 遍历设备上的所有 Plane，筛选属于当前 CRTC 的并打印属性
  for (i = 0; i < pDrmDsp->dev->num_planes; i++) {
    struct sp_plane* plane = &pDrmDsp->dev->planes[i];
    drmModePlanePtr plane_ptr = plane->plane;

    // 检查该 Plane 是否可以绑定到当前的 CRTC
    if (!(plane_ptr->possible_crtcs & (1 << pDrmDsp->test_crtc->pipe))) {
      continue;
    }

    // 动态查询 Plane 的 type 属性
    uint32_t plane_type = DRM_PLANE_TYPE_OVERLAY; // 默认值
    uint32_t rotation_pid = 0;
    uint32_t in_formats_pid = 0;

    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(pDrmDsp->dev->fd, plane_ptr->plane_id, DRM_MODE_OBJECT_PLANE);
    if (props) {
      for (uint32_t j = 0; j < props->count_props; j++) {
        drmModePropertyPtr prop = drmModeGetProperty(pDrmDsp->dev->fd, props->props[j]);
        if (!prop) continue;
        if (strcmp(prop->name, "type") == 0) {
          plane_type = props->prop_values[j];
        } else if (strcmp(prop->name, "rotation") == 0) {
          rotation_pid = prop->prop_id;
        } else if (strcmp(prop->name, "IN_FORMATS") == 0) {
          in_formats_pid = prop->prop_id;
        }
        drmModeFreeProperty(prop);
      }
      drmModeFreeObjectProperties(props);
    }

    printf("[Plane %d] id: %u, type: %s\n", i, plane_ptr->plane_id,
           (plane_type == DRM_PLANE_TYPE_PRIMARY) ? "Primary" :
           (plane_type == DRM_PLANE_TYPE_OVERLAY) ? "Overlay" : "Cursor");
  }

  /* Select first NV12-capable plane (Smart0 - no HW rotation, RGA handles it) */
  pDrmDsp->test_plane = NULL;
  for (i = 0; i < pDrmDsp->num_test_planes; i++) {
    if (is_supported_format(pDrmDsp->plane[i], DRM_FORMAT_NV12)) {
      pDrmDsp->test_plane = pDrmDsp->plane[i];
      break;
    }
  }
  if (!pDrmDsp->test_plane) {
    printf("No NV12-capable plane found\n");
    return -1;
  }
  printf("Selected plane: id=%d\n", pDrmDsp->test_plane->plane->plane_id);

#if ISPDEMO_ATOMIC_NONBLOCK
  /* 查询 plane 的属性 ID，供原子提交使用 */
  {
    drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(
        pDrmDsp->dev->fd, pDrmDsp->test_plane->plane->plane_id, DRM_MODE_OBJECT_PLANE);
    if (props) {
      for (i = 0; i < props->count_props; i++) {
        drmModePropertyPtr p = drmModeGetProperty(pDrmDsp->dev->fd, props->props[i]);
        if (p) {
          if (!strcmp(p->name, "FB_ID"))       s_atomic_prop_fb_id = props->props[i];
          else if (!strcmp(p->name, "CRTC_ID")) s_atomic_prop_crtc_id = props->props[i];
          else if (!strcmp(p->name, "SRC_X"))  s_atomic_prop_src_x = props->props[i];
          else if (!strcmp(p->name, "SRC_Y"))  s_atomic_prop_src_y = props->props[i];
          else if (!strcmp(p->name, "SRC_W"))  s_atomic_prop_src_w = props->props[i];
          else if (!strcmp(p->name, "SRC_H"))  s_atomic_prop_src_h = props->props[i];
          else if (!strcmp(p->name, "DST_X"))  s_atomic_prop_dst_x = props->props[i];
          else if (!strcmp(p->name, "DST_Y"))  s_atomic_prop_dst_y = props->props[i];
          else if (!strcmp(p->name, "DST_W"))  s_atomic_prop_dst_w = props->props[i];
          else if (!strcmp(p->name, "DST_H"))  s_atomic_prop_dst_h = props->props[i];
          drmModeFreeProperty(p);
        }
      }
      drmModeFreeObjectProperties(props);
    }
    printf("[atomic] props: fb=%u crtc=%u src=%u,%u,%u,%u dst=%u,%u,%u,%u\n",
           s_atomic_prop_fb_id, s_atomic_prop_crtc_id,
           s_atomic_prop_src_x, s_atomic_prop_src_y, s_atomic_prop_src_w, s_atomic_prop_src_h,
           s_atomic_prop_dst_x, s_atomic_prop_dst_y, s_atomic_prop_dst_w, s_atomic_prop_dst_h);
  }
#endif

#endif

#if ISPDEMO_ENABLE_RGA
  rkRgaInit();
#endif
    printf("finish initDrmDsp\n");

  return ret;
}

void deInitDrmDsp() {
  struct drmDsp* pDrmDsp = &gDrmDsp;
  if (pDrmDsp->bo[0])
    free_sp_bo(pDrmDsp->bo[0]);
  if (pDrmDsp->bo[1])
    free_sp_bo(pDrmDsp->bo[1]);
  destroy_sp_dev(pDrmDsp->dev);
  memset(pDrmDsp, 0, sizeof(struct drmDsp));
}

static int arm_camera_yuv420_scale_arm(char *srcbuf, char *dstbuf,int src_w, int src_h,int dst_w, int dst_h) {
	unsigned char *psY = NULL,*pdY = NULL,*psUV = NULL,*pdUV = NULL;
	// unsigned char *src,*dst;
	int srcW,srcH,cropW,cropH,dstW,dstH;
	long zoomindstxIntInv,zoomindstyIntInv;
	long x,y;
	long yCoeff00,yCoeff01,xCoeff00,xCoeff01;
	long sX,sY;
	long r0,r1,a,b,c,d;
	int nv21DstFmt = 0, mirror = 0;
	int ratio = 0;
	int top_offset=0,left_offset=0;

	//need crop ?
	if((src_w*100/src_h) != (dst_w*100/dst_h)){
		ratio = ((src_w*100/dst_w) >= (src_h*100/dst_h))?(src_h*100/dst_h):(src_w*100/dst_w);
		cropW = ratio*dst_w/100;
		cropH = ratio*dst_h/100;

		left_offset=((src_w-cropW)>>1) & (~0x01);
		top_offset=((src_h-cropH)>>1) & (~0x01);
	}else{
  /* FORCE: pick highest-index plane available to CRTC (expected Cluster0) */
		top_offset=0;
		left_offset=0;
	}

	// src = psY = (unsigned char*)(srcbuf)+top_offset*src_w+left_offset;
	//psUV = psY +src_w*src_h+top_offset*src_w/2+left_offset;
	psUV = (unsigned char*)(srcbuf) +src_w*src_h+top_offset*src_w/2+left_offset;


	srcW =src_w;
	srcH = src_h;
//	cropW = src_w;
//	cropH = src_h;


	// dst = pdY = (unsigned char*)dstbuf;
	pdUV = pdY + dst_w*dst_h;
	dstW = dst_w;
	dstH = dst_h;

	zoomindstxIntInv = ((unsigned long)(cropW)<<16)/dstW + 1;
	zoomindstyIntInv = ((unsigned long)(cropH)<<16)/dstH + 1;
	//y
	//for(y = 0; y<dstH - 1 ; y++ ) {
	for(y = 0; y<dstH; y++ ) {
		yCoeff00 = (y*zoomindstyIntInv)&0xffff;
		yCoeff01 = 0xffff - yCoeff00;
		sY = (y*zoomindstyIntInv >> 16);
		sY = (sY >= srcH - 1)? (srcH - 2) : sY;
		for(x = 0; x<dstW; x++ ) {
			xCoeff00 = (x*zoomindstxIntInv)&0xffff;
			xCoeff01 = 0xffff - xCoeff00;
			sX = (x*zoomindstxIntInv >> 16);
			sX = (sX >= srcW -1)?(srcW- 2) : sX;
			a = psY[sY*srcW + sX];
			b = psY[sY*srcW + sX + 1];
			c = psY[(sY+1)*srcW + sX];
			d = psY[(sY+1)*srcW + sX + 1];

			r0 = (a * xCoeff01 + b * xCoeff00)>>16 ;
			r1 = (c * xCoeff01 + d * xCoeff00)>>16 ;
			r0 = (r0 * yCoeff01 + r1 * yCoeff00)>>16;

			if(mirror)
				pdY[dstW -1 - x] = r0;
			else
				pdY[x] = r0;
		}
		pdY += dstW;
	}

	dstW /= 2;
	dstH /= 2;
	srcW /= 2;
	srcH /= 2;

	//UV
	//for(y = 0; y<dstH - 1 ; y++ ) {
	for(y = 0; y<dstH; y++ ) {
		yCoeff00 = (y*zoomindstyIntInv)&0xffff;
		yCoeff01 = 0xffff - yCoeff00;
		sY = (y*zoomindstyIntInv >> 16);
		sY = (sY >= srcH -1)? (srcH - 2) : sY;
		for(x = 0; x<dstW; x++ ) {
			xCoeff00 = (x*zoomindstxIntInv)&0xffff;
			xCoeff01 = 0xffff - xCoeff00;
			sX = (x*zoomindstxIntInv >> 16);
			sX = (sX >= srcW -1)?(srcW- 2) : sX;
			//U
			a = psUV[(sY*srcW + sX)*2];
			b = psUV[(sY*srcW + sX + 1)*2];
			c = psUV[((sY+1)*srcW + sX)*2];
			d = psUV[((sY+1)*srcW + sX + 1)*2];

			r0 = (a * xCoeff01 + b * xCoeff00)>>16 ;
			r1 = (c * xCoeff01 + d * xCoeff00)>>16 ;
			r0 = (r0 * yCoeff01 + r1 * yCoeff00)>>16;

			if(mirror && nv21DstFmt)
				pdUV[dstW*2-1- (x*2)] = r0;
			else if(mirror)
				pdUV[dstW*2-1-(x*2+1)] = r0;
			else if(nv21DstFmt)
				pdUV[x*2 + 1] = r0;
			else
				pdUV[x*2] = r0;
			//V
			a = psUV[(sY*srcW + sX)*2 + 1];
			b = psUV[(sY*srcW + sX + 1)*2 + 1];
			c = psUV[((sY+1)*srcW + sX)*2 + 1];
			d = psUV[((sY+1)*srcW + sX + 1)*2 + 1];

			r0 = (a * xCoeff01 + b * xCoeff00)>>16 ;
			r1 = (c * xCoeff01 + d * xCoeff00)>>16 ;
			r0 = (r0 * yCoeff01 + r1 * yCoeff00)>>16;

			if(mirror && nv21DstFmt)
				pdUV[dstW*2-1- (x*2+1) ] = r0;
			else if(mirror)
				pdUV[dstW*2-1-(x*2)] = r0;
			else if(nv21DstFmt)
				pdUV[x*2] = r0;
			else
				pdUV[x*2 + 1] = r0;
		}
		pdUV += dstW*2;
	}
	return 0;
}

int drmDspFrame(int srcWidth, int srcHeight, int dispWidth, int dispHeight,
		int dmaFd, void* srcAddr, int fmt)
{
  int ret;
  // struct drm_mode_create_dumb cd;
  struct sp_bo* bo;
  struct drmDsp* pDrmDsp = &gDrmDsp;

  int crtc_w = pDrmDsp->test_crtc->crtc->mode.hdisplay;
  int crtc_h = pDrmDsp->test_crtc->crtc->mode.vdisplay;
  int wAlign16 = ((crtc_w + 15) & (~15));
  int hAlign16 = (crtc_h + 15) & (~15);
  // int frameSize = wAlign16 * hAlign16 * 3 / 2;
  uint32_t handles[4], pitches[4], offsets[4];

  if (DRM_FORMAT_NV12 != fmt) {
    printf("%s just support NV12 to display\n", __func__);
    return -1;
  }
  //create bo
#if 1
  if (!pDrmDsp->bo[0]) {
    printf("%s:bo widthxheight:%dx%d\n", __func__, wAlign16, hAlign16);
    pDrmDsp->bo[0] = create_sp_bo(pDrmDsp->dev, wAlign16, hAlign16,
                                  16, 32, DRM_FORMAT_NV12, 0);
    pDrmDsp->bo[1] = create_sp_bo(pDrmDsp->dev, wAlign16, hAlign16,
                                  16, 32, DRM_FORMAT_NV12, 0);
    if (!pDrmDsp->bo[0] || !pDrmDsp->bo[1]) {
      printf("%s:create bo failed ! \n", __func__);
      return -1;
    }
    pDrmDsp->nextbo = pDrmDsp->bo[0];
    /* 填黑底：Y=0, UV=128，保证旋转后的黑边是纯黑（而非残留数据） */
    memset(pDrmDsp->bo[0]->map_addr, 0, wAlign16 * hAlign16);
    memset(pDrmDsp->bo[0]->map_addr + wAlign16 * hAlign16, 128, wAlign16 * hAlign16 / 2);
    memset(pDrmDsp->bo[1]->map_addr, 0, wAlign16 * hAlign16);
    memset(pDrmDsp->bo[1]->map_addr + wAlign16 * hAlign16, 128, wAlign16 * hAlign16 / 2);
  }

  if (!pDrmDsp->nextbo) {
    printf("%s:no available bo ! \n", __func__);
    return -1;
  }

  bo = pDrmDsp->nextbo;
#else
  bo = create_sp_bo(pDrmDsp->dev, wAlign16, hAlign16,
                    16, 32, DRM_FORMAT_NV12, 0);
  if (!bo)
    printf("%s:create bo failed ! \n", __func__);
#endif

  handles[0] = bo->handle;
  pitches[0] = wAlign16;
  offsets[0] = 0;
  handles[1] = bo->handle;
  pitches[1] = wAlign16;
  offsets[1] = wAlign16 * hAlign16; /* Y plane size = BO width x BO height, fixes UV offset for non-16:10 resolutions */

#if ISPDEMO_ENABLE_RGA
    /* RGA: rotate 270 camera 1280x800 -> display 800x1280 */
  rkRgaBlitRaw(dmaFd, srcWidth, srcHeight, RK_FORMAT_YCrCb_420_SP,
               bo->fd, bo->width, bo->height, 0x07);
#else
    //copy src data to bo
  if (srcWidth == dispWidth)
	  memcpy(bo->map_addr, srcAddr, wAlign16 * hAlign16 * 3 / 2);
  else
	  arm_camera_yuv420_scale_arm(srcAddr, bo->map_addr, srcWidth, srcHeight, dispWidth, dispHeight);
#endif
  
  ret = drmModeAddFB2(bo->dev->fd, bo->width, bo->height,
                      bo->format, handles, pitches, offsets,
                      &bo->fb_id, bo->flags);
    if (ret) {
    printf("%s:failed to create fb ret=%d\n", __func__, ret);
    printf("fd:%d ,wxh:%ux%u,format:%u,handles:%u,%u,pictches:%u,%u,offsets:%u,%u,fb_id:%u,flags:%u \n",
           bo->dev->fd, bo->width, bo->height, bo->format,
           handles[0], handles[1], pitches[0], pitches[1],
           offsets[0], offsets[1], bo->fb_id, bo->flags);
    return ret;
  }

#if ISPDEMO_ATOMIC_NONBLOCK
  /* 原子非阻塞提交：不等 vblank，立即返回，消除 ~10ms 同步阻塞。
     注意：本板 plane 未暴露 DST_* 属性，atomic 提交不设 DST。
     首帧必须用 drmModeSetPlane 先建立 plane 的 DST 状态，否则 plane 不可见（黑屏）。
     atomic 失败时回退 SetPlane 保证显示。 */
  {
    static int _first_frame = 1;
    if (_first_frame) {
      /* 首帧：SetPlane 建立 plane 状态（含 DST），之后 atomic 沿用该状态 */
      ret = drmModeSetPlane(pDrmDsp->dev->fd, pDrmDsp->test_plane->plane->plane_id,
                            pDrmDsp->test_crtc->crtc->crtc_id, bo->fb_id, 0, 0, 0,
                            pDrmDsp->test_crtc->crtc->mode.hdisplay, pDrmDsp->test_crtc->crtc->mode.vdisplay,
                            0, 0, pDrmDsp->test_crtc->crtc->mode.hdisplay << 16, pDrmDsp->test_crtc->crtc->mode.vdisplay << 16);
      _first_frame = 0;
      if (ret) {
        printf("first SetPlane failed ret=%d\n", ret);
        return ret;
      }
    } else {
      drmModeAtomicReq *req = drmModeAtomicAlloc();
      if (req) {
        uint32_t _pid = pDrmDsp->test_plane->plane->plane_id;
        drmModeAtomicAddProperty(req, _pid, s_atomic_prop_crtc_id, pDrmDsp->test_crtc->crtc->crtc_id);
        drmModeAtomicAddProperty(req, _pid, s_atomic_prop_fb_id, bo->fb_id);
        drmModeAtomicAddProperty(req, _pid, s_atomic_prop_src_x, 0);
        drmModeAtomicAddProperty(req, _pid, s_atomic_prop_src_y, 0);
        drmModeAtomicAddProperty(req, _pid, s_atomic_prop_src_w, bo->width << 16);
        drmModeAtomicAddProperty(req, _pid, s_atomic_prop_src_h, bo->height << 16);
        ret = drmModeAtomicCommit(pDrmDsp->dev->fd, req, DRM_MODE_ATOMIC_NONBLOCK, NULL);
        drmModeAtomicFree(req);
        if (ret) {
          printf("failed to atomic commit ret=%d, fallback SetPlane\n", ret);
          ret = drmModeSetPlane(pDrmDsp->dev->fd, pDrmDsp->test_plane->plane->plane_id,
                                pDrmDsp->test_crtc->crtc->crtc_id, bo->fb_id, 0, 0, 0,
                                pDrmDsp->test_crtc->crtc->mode.hdisplay, pDrmDsp->test_crtc->crtc->mode.vdisplay,
                                0, 0, pDrmDsp->test_crtc->crtc->mode.hdisplay << 16, pDrmDsp->test_crtc->crtc->mode.vdisplay << 16);
        }
      }
    }
  }
#else
  ret = drmModeSetPlane(pDrmDsp->dev->fd, pDrmDsp->test_plane->plane->plane_id,
                        pDrmDsp->test_crtc->crtc->crtc_id, bo->fb_id, 0, 0, 0,
                        //pDrmDsp->test_crtc->crtc->mode.hdisplay,
			pDrmDsp->test_crtc->crtc->mode.hdisplay, pDrmDsp->test_crtc->crtc->mode.vdisplay,
                        //pDrmDsp->test_crtc->crtc->mode.vdisplay,
                        0, 0, pDrmDsp->test_crtc->crtc->mode.hdisplay << 16, pDrmDsp->test_crtc->crtc->mode.vdisplay << 16);
  if (ret) {
    printf("failed to set plane to crtc ret=%d\n", ret);
    return ret;
  }
#endif
  //free_sp_bo(bo);
#if 0
  if (pDrmDsp->test_plane->bo) {
    if (pDrmDsp->test_plane->bo->fb_id) {
      ret = drmModeRmFB(pDrmDsp->dev->fd, pDrmDsp->test_plane->bo->fb_id);
      if (ret)
        printf("Failed to rmfb ret=%d!\n", ret);
    }
    if (pDrmDsp->test_plane->bo->handle) {
      struct drm_gem_close req = {
        .handle = pDrmDsp->test_plane->bo->handle,
      };

      drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_CLOSE, &req);
      printf("%s:close bo success!\n", __func__);
    }

    if (!pDrmDsp->nextbo)
      free_sp_bo(pDrmDsp->test_plane->bo);
  }
  pDrmDsp->test_plane->bo = bo; //last po
#endif
#if 1
  //switch bo
  if (pDrmDsp->nextbo == pDrmDsp->bo[0])
    pDrmDsp->nextbo = pDrmDsp->bo[1];
  else
    pDrmDsp->nextbo = pDrmDsp->bo[0];
#endif
  return ret;
}
