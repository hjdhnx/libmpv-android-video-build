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

# libxml2 静态库（供 ffmpeg 的 dash demuxer 使用：dash_demuxer_deps="libxml2"）。
# 交叉编译要点：autotools + NDK toolchain；禁用 Python/ICU/HTTP/zlib 等非必需
# 部件（DASH 只需 XML 解析与 MPD 读入）。
../configure \
	--host=$ndk_triple \
	--prefix=$prefix_dir \
	--enable-static \
	--disable-shared \
	--without-python \
	--without-lzma \
	--without-zlib \
	--without-iconv \
	--without-http \
	--without-ftp \
	--disable-dependency-tracking

make -j$cores
make install
