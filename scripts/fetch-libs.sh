#!/usr/bin/env bash
#
# fetch-libs.sh — 从本地 Rockchip SDK 拷贝闭源 .so 到 demo/libs，并重建 SDK 符号链接
#
# 为什么需要它？
#   demo/libs/arm64 下的 librknnrt.so / librga.so / libdrm.so / librockchip_mpp.so
#   是 Rockchip 的闭源二进制，本项目无权再分发，因此不随仓库提交。
#   任何想构建本项目的用户，其本地都有 Rockchip SDK（编译驱动/ISP 必需），
#   跑本脚本即可把这些库从自己的 SDK 里拷进来。
#
# 用法:
#   ./scripts/fetch-libs.sh <SDK_ROOT>
#
#   SDK_ROOT:  Rockchip SDK 根目录，例如 ~/rk-linux6.1-2026060111/rk356x-linux-2026060111
#
# 环境变量:
#   ARCH=arm|aarch64   目标架构，默认 aarch64

set -u

SDK_ROOT="${1:?用法: $0 <Rockchip SDK 根目录>}"
ARCH="${ARCH:-aarch64}"

# 项目内 libs 目录
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DEST_DIR="$REPO_ROOT/demo/libs/$ARCH"
mkdir -p "$DEST_DIR"

# 闭源库 -> SDK 内候选路径（不同 SDK 版本路径略有差异，找不到会再全库搜索）
declare -A LIBS
LIBS[librknnrt.so]="external/rknpu2/runtime/RK356X/Linux/librknn_api/aarch64/librknnrt.so
                    external/rknpu2/runtime/Linux/librknn_api/aarch64/librknnrt.so"
LIBS[librga.so]="external/rknpu2/examples/3rdparty/rga/RK356X/lib/Linux/aarch64/librga.so
                 external/linux-rga/build/lib/librga.so"
LIBS[libdrm.so]="external/linux-rga/samples/utils/3rdparty/libdrm/lib/arm64/libdrm.so
                 external/drm_util/build/libdrm.so"
LIBS[librockchip_mpp.so]="external/rknpu2/examples/3rdparty/mpp/Linux/aarch64/librockchip_mpp.so
                          external/mpp/build/linux/aarch64/librockchip_mpp.so"

find_in_sdk() {
    local name="$1"
    # 先试候选路径
    local cand
    while read -r cand; do
        [ -z "$cand" ] && continue
        if [ -f "$SDK_ROOT/$cand" ]; then
            echo "$SDK_ROOT/$cand"
            return 0
        fi
    done <<< "${LIBS[$name]:-}"
    # 再全库搜索（限定典型目录，避免扫到无关文件）
    local found
    found=$(find "$SDK_ROOT/external" -maxdepth 8 -name "$name" 2>/dev/null | head -n 1)
    if [ -n "$found" ]; then
        echo "$found"
        return 0
    fi
    return 1
}

echo "== 目标目录: $DEST_DIR =="
local_sym() { [ -f "$DEST_DIR/$1" ]; }

for name in librknnrt.so librga.so libdrm.so librockchip_mpp.so; do
    if local_sym "$name"; then
        echo "✓ 已存在 $name（跳过）"
        continue
    fi
    src="$(find_in_sdk "$name")"
    if [ -n "$src" ]; then
        cp -v "$src" "$DEST_DIR/$name"
    else
        echo "✗ 未在 SDK 中找到 $name，请手动拷贝到 $DEST_DIR/"
    fi
done

# libgstrtspserver-1.0.so 是 LGPL 开源库，不强制从 SDK 拿。
if local_sym libgstrtspserver-1.0.so; then
    echo "✓ 已存在 libgstrtspserver-1.0.so（跳过）"
else
    echo "提示: 缺少 libgstrtspserver-1.0.so（LGPL）。三种获取方式："
    echo "  1) 系统包:   sudo apt install libgstrtspserver-1.0-0（并链接开发头文件）"
    echo "  2) 源码构建: 从 https://github.com/GStreamer/gst-rtsp-server 构建 1.16.x 后拷贝"
    echo "  3) SDK:      find <SDK> -name 'libgstrtspserver*.so*' 找到后拷贝到 $DEST_DIR/"
    echo "  头文件已随仓库提供: demo/libs/include/gstreamer-1.0/gst/rtsp-server/（来自 1.16.3）"
fi

# 重建 SDK 符号链接（本仓库 clone 后这些链接会丢失；若构建用不到可忽略）
link_missing=""
for target in \
    "$SDK_ROOT/external/linux-rga/include" \
    "$SDK_ROOT/external/camera_engine_rkaiq/rk_aiq/include/common/rk_aiq_mems_sensor.h"; do
    [ -e "$target" ] || continue
    if [ "$(basename "$target")" = "include" ]; then
        link_dst="$REPO_ROOT/demo/include/rga"
        [ -e "$link_dst" ] && continue
        ln -s "$target" "$link_dst"
        echo "✓ 重建符号链接 demo/include/rga -> $target"
    else
        link_dst="$REPO_ROOT/demo/iio/include/rk_aiq_mems_sensor.h"
        [ -e "$link_dst" ] && continue
        ln -s "$target" "$link_dst"
        echo "✓ 重建符号链接 demo/iio/include/rk_aiq_mems_sensor.h"
    fi
done

echo
echo "完成。缺失的库请手动补齐后，再执行 scripts/build.sh。"
