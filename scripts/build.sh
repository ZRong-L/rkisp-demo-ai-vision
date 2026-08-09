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

echo "== 配置构建 (ARCH=$ARCH, 工具链=$TOOLCHAIN_ROOT) =="
cmake -S . -B "$BUILD_DIR" \
    -DARCH="$ARCH" \
    -DCMAKE_BUILD_TYPE=Release \
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
