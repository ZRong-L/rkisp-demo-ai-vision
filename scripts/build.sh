#!/usr/bin/env bash
#
# build.sh — 交叉编译 rkisp_demo
#
# 用法:
#   ./scripts/build.sh [sdk_root]
#
# 说明:
#   - 本项目依赖 Rockchip SDK（rkaiq installed 目录、闭源 .so 等），
#     建议在 SDK 内执行（即本仓库位于 external/camera_engine_rkaiq/rkisp_demo）。
#   - 闭源库请先执行 ./scripts/fetch-libs.sh <SDK_ROOT> 补齐。
#
# 环境变量（可覆盖）:
#   TOOLCHAIN_ROOT   交叉工具链根目录，默认 /opt/gcc-arm/gcc-arm-9.2-2019.12-x86_64-aarch64-none-linux-gnu
#   ARCH             目标架构，默认 aarch64
#   BUILD_DIR        构建输出目录，默认 build_local
#   ISP_HW_VERSION   ISP 硬件版本（默认 -DISP_HW_V21，对应 ISP21/RK3568；V32 用 -DISP_HW_V32）
#   RKAIQ_TARGET_SOC 目标 SoC（默认 rk356x）
#   CXX_STANDARD_LIBS 附加链接库（默认覆盖 CMAKE_CXX_STANDARD_LIBRARIES，
#                     包含 rknn/mpp/gstreamer/rtsp-server 等，见下方说明）
#
# 依赖 rkaiq installed 目录默认取 ../rk_aiq/build/linux/output/<ARCH>/installed，
# 可用 -DRKAIQ_INSTALLED_DIR=<path> 覆盖（见 CMakeLists.txt）。

set -euo pipefail

SDK_ROOT="${1:-${SDK_ROOT:-}}"
if [ -n "$SDK_ROOT" ] && [ ! -d "$SDK_ROOT" ]; then
    echo "错误: SDK_ROOT 不存在: $SDK_ROOT" >&2
    exit 1
fi

TOOLCHAIN_ROOT="${TOOLCHAIN_ROOT:-/opt/gcc-arm/gcc-arm-9.2-2019.12-x86_64-aarch64-none-linux-gnu}"
ARCH="${ARCH:-aarch64}"
BUILD_DIR="${BUILD_DIR:-build_local}"
ISP_HW_VERSION="${ISP_HW_VERSION:--DISP_HW_V21}"
RKAIQ_TARGET_SOC="${RKAIQ_TARGET_SOC:-rk356x}"
# 覆盖 CMAKE_CXX_STANDARD_LIBRARIES：rkaiq 库不自动带 gstreamer/rtsp 链接，
# 以 -l 方式直接追加到链接行（与既有的 demo 构建方式保持一致）。
CXX_STANDARD_LIBS="${CXX_STANDARD_LIBS:--lrga -lrknnrt -lrockchip_mpp -lgstrtspserver-1.0 -lgstreamer-1.0 -lgobject-2.0 -lglib-2.0 -lgstbase-1.0 -lgstvideo-1.0 -ldrm}"

if [ "$ARCH" = "aarch64" ]; then
    TRIPLE="aarch64-none-linux-gnu"
else
    TRIPLE="arm-none-linux-gnueabihf"
fi

GCC="$TOOLCHAIN_ROOT/bin/$TRIPLE-gcc"
GXX="$TOOLCHAIN_ROOT/bin/$TRIPLE-g++"
if [ ! -x "$GCC" ] || [ ! -x "$GXX" ]; then
    echo "错误: 找不到交叉工具链，请设置 TOOLCHAIN_ROOT，或安装:"
    echo "  apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu"
    echo "  并设置 TOOLCHAIN_ROOT=/usr"
    echo "  （注意：不同工具链 glibc 版本需与板端 rootfs 兼容，详见 README）"
    exit 1
fi

if [ -n "$SDK_ROOT" ]; then
    echo "== 检查闭源库 =="
    ./scripts/fetch-libs.sh "$SDK_ROOT"
fi

echo "== 配置构建 (ARCH=$ARCH, ISP=$ISP_HW_VERSION, 工具链=$TOOLCHAIN_ROOT) =="
cmake -S . -B "$BUILD_DIR" \
    -DARCH="$ARCH" \
    -DISP_HW_VERSION="$ISP_HW_VERSION" \
    -DRKAIQ_TARGET_SOC="$RKAIQ_TARGET_SOC" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_STANDARD_LIBRARIES="$CXX_STANDARD_LIBS" \
    -DCMAKE_C_COMPILER="$GCC" \
    -DCMAKE_CXX_COMPILER="$GXX" \
    -DCMAKE_STRIP="$TOOLCHAIN_ROOT/bin/$TRIPLE-strip" \
    -DCMAKE_INSTALL_PREFIX="$PWD/deploy"

echo "== 编译 =="
cmake --build "$BUILD_DIR" -j"$(nproc)"

echo
echo "完成。产物: $BUILD_DIR/exe/Release/rkisp_demo"
echo "部署到板子后运行示例:"
echo "  ./rkisp_demo --device /dev/video0 --width 1280 --height 800 --count 2000 --vop \\"
echo "                --model ./model/yolov5s-640-640.rknn"
