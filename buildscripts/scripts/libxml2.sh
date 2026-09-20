#!/bin/bash -e

. ../../include/depinfo.sh
. ../../include/path.sh

build=_build$ndk_suffix

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf $build
	exit 0
else
	exit 255
fi

$0 clean # separate building not supported, always clean

mkdir -p $build
cd $build

# libxml2 静态库（ffmpeg dash demuxer 的硬依赖：dash_demuxer_deps="libxml2"）。
#
# 用 NDK 官方 CMake toolchain（git 源码无现成 configure；autogen 需完整
# autotools 链，而 CMakeLists 是上游一等公民、交叉编译更省事）。
# 关闭非必需部件：Python 绑定 / lzma / zlib / iconv / HTTP / FTP —— DASH 只需
# XML 解析与 MPD 读入（减体积，也减少交叉编译依赖面）。
# NDK 根：优先用 ANDROID_HOME/ndk/<版本>（CI 已导出且版本号可知）；
# 退化用 clang 路径推导（<ndk>/toolchains/llvm/prebuilt/<host>/bin/clang → 上溯 5 级）
ndk_root=""
if [ -n "$ANDROID_HOME" ] && [ -d "$ANDROID_HOME/ndk/$v_ndk" ]; then
	ndk_root="$ANDROID_HOME/ndk/$v_ndk"
elif [ -n "$ANDROID_NDK_HOME" ] && [ -d "$ANDROID_NDK_HOME" ]; then
	ndk_root="$ANDROID_NDK_HOME"
else
	ndk_root="$(cd "$(dirname "$(command -v clang)")/../../../../.." && pwd)"
fi
toolchain="$ndk_root/build/cmake/android.toolchain.cmake"
[ -f "$toolchain" ] || { echo "找不到 NDK toolchain: $toolchain"; exit 1; }

cmake .. \
	-DCMAKE_TOOLCHAIN_FILE="$toolchain" \
	-DANDROID_ABI="$prefix_name" \
	-DANDROID_PLATFORM=android-24 \
	-DCMAKE_INSTALL_PREFIX="$prefix_dir" \
	-DBUILD_SHARED_LIBS=OFF \
	-DLIBXML2_WITH_PYTHON=OFF \
	-DLIBXML2_WITH_LZMA=OFF \
	-DLIBXML2_WITH_ZLIB=OFF \
	-DLIBXML2_WITH_ICONV=OFF \
	-DLIBXML2_WITH_HTTP=OFF \
	-DLIBXML2_WITH_FTP=OFF \
	-DLIBXML2_WITH_TESTS=OFF \
	-DLIBXML2_WITH_PROGRAMS=OFF

make -j$cores
make DESTDIR="$prefix_dir" install
