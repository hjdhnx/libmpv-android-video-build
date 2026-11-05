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

unset CC CXX # meson wants these unset

# ---- ByteVC2（仅 arm64，与 flavors/default.sh 的门控一致）：libavcodec.a 里
# bytevc2dec 的 UND 符号需在 libmpv 链接期解析——ffmpeg 静态+无程序形态下
# --extra-libs 不产生任何链接动作（CI 实锤：UND 无 DT_NEEDED），必须在此把
# 解码库 .so 显式喂给链接器，产出 DT_NEEDED libbyteVC2dec.so（SONAME 恰为
# 该名，消费方 jniLibs 随带同名文件即可被 Android linker 递归加载）。----
bvc2_link_args=()
if [[ "$ndk_triple" == "aarch64"* ]] && [ -f "$DIR/bytevc2/libbyteVC2dec.so" ]; then
	bvc2_link_args=(-Dc_link_args="$DIR/bytevc2/libbyteVC2dec.so")
fi

meson setup $build --cross-file "$prefix_dir"/crossfile.txt \
	--strip \
	--prefer-static \
	--default-library shared \
	-Dgpl=false \
	-Dlibmpv=true \
	-Dbuild-date=false \
 	-Dlua=disabled \
 	-Dcplayer=false \
	-Diconv=disabled \
	-Dvulkan=disabled \
 	-Dmanpage-build=disabled \
	"${bvc2_link_args[@]}"

ninja -C $build -j$cores
DESTDIR="$prefix_dir" ninja -C $build install

ln -sf "$prefix_dir"/lib/libmpv.so "$native_dir"
