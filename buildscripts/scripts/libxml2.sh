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
# clang 位于 <ndk>/toolchains/llvm/prebuilt/<host>/bin/clang —— 上溯 4 级到 NDK 根
ndk_root="$(cd "$(dirname "$(command -v clang)")/../../../.." && pwd)"
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
