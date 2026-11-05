#!/bin/bash -e

. ../../include/depinfo.sh
. ../../include/path.sh

if [ "$1" == "build" ]; then
	true
elif [ "$1" == "clean" ]; then
	rm -rf _build$ndk_suffix
	exit 0
else
	exit 255
fi

mkdir -p _build$ndk_suffix
cd _build$ndk_suffix

cpu=armv7-a
[[ "$ndk_triple" == "aarch64"* ]] && cpu=armv8-a
[[ "$ndk_triple" == "x86_64"* ]] && cpu=generic
[[ "$ndk_triple" == "i686"* ]] && cpu="i686 --disable-asm"

cpuflags=
[[ "$ndk_triple" == "arm"* ]] && cpuflags="$cpuflags -mfpu=neon -mcpu=cortex-a8"

# ---- ByteVC2 (B 站 bv2C 私有编码) 软解接入，仅 arm64（libbyteVC2dec.so 仅
# arm64 版；其它架构缺库 configure 必挂）。flavor 会被 cp 成 scripts/ffmpeg.sh
# 在 deps/ffmpeg/_build 里执行——cwd 已变，定位必须用 path.sh 的绝对路径
# $DIR（= buildscripts），勿用 BASH_SOURCE 相对路径（CI 实锤 cd 失败）。
# ffmpeg 树 = ../（_build 的父目录）；补丁脚本幂等。
# 源码补丁（mov.c bv2C 切换等）**全架构应用**（幂等无害）——x86_64 prefix
# 的 libavformat 也含切换逻辑，供下方 demuxer 探针验证；解码器 enable/链接
# 仅 arm64。----
BVC2_DIR="$DIR/bytevc2"
BVC2_FLAGS=
BVC2_LDFLAG=
python3 "$BVC2_DIR/apply.py" ..
if [[ "$ndk_triple" == "aarch64"* ]] && [ -f "$BVC2_DIR/libbyteVC2dec.so" ]; then
	BVC2_LDFLAG="-L$BVC2_DIR"
	BVC2_FLAGS="--enable-decoder=bytevc2 --extra-libs=-lbyteVC2dec"
fi

../configure \
	--target-os=android --enable-cross-compile --cross-prefix=$ndk_triple- --ar=$AR --cc=$CC --ranlib=$RANLIB \
	--arch=${ndk_triple%%-*} --cpu=$cpu --pkg-config=pkg-config --nm=llvm-nm \
	--extra-cflags="-I$prefix_dir/include $cpuflags" --extra-ldflags="-L$prefix_dir/lib $BVC2_LDFLAG" \
	\
	--disable-gpl \
	--disable-nonfree \
	--enable-version3 \
	--enable-static \
	--disable-shared \
	--disable-vulkan \
	--disable-iconv \
	--pkg-config-flags=--static \
	\
	--disable-muxers \
	--disable-decoders \
	--disable-encoders \
	--disable-demuxers \
	--disable-parsers \
	--disable-protocols \
	--disable-devices \
	--disable-filters \
	--disable-doc \
	--disable-avdevice \
	--disable-programs \
	--disable-gray \
	--disable-swscale-alpha \
	\
	--enable-jni \
	--enable-bsfs \
	--enable-mediacodec \
	\
	--disable-dxva2 \
	--disable-vaapi \
	--disable-vdpau \
	--disable-bzlib \
	--disable-linux-perf \
	--disable-videotoolbox \
	--disable-audiotoolbox \
	\
	--enable-small \
	--enable-hwaccels \
	--enable-optimizations \
	--enable-runtime-cpudetect \
	\
	--enable-mbedtls \
	\
	--enable-libdav1d \
	\
	--enable-avutil \
	--enable-avcodec \
	--enable-avfilter \
	--enable-avformat \
	--enable-swscale \
	--enable-swresample \
	\
	--enable-libwebp \
	\
	--enable-decoder=flv \
	--enable-decoder=h263 \
	--enable-decoder=h263i \
	--enable-decoder=h263p \
	--enable-decoder=h264* \
	--enable-decoder=mpeg1video \
	--enable-decoder=mpeg2* \
	--enable-decoder=mpeg4* \
	--enable-decoder=vp6 \
	--enable-decoder=vp6a \
	--enable-decoder=vp6f \
	--enable-decoder=vp8* \
	--enable-decoder=vp9* \
	--enable-decoder=hevc* \
	--enable-decoder=av1* \
	--enable-decoder=libdav1d \
	--enable-decoder=theora \
	--enable-decoder=msmpeg* \
	--enable-decoder=wmv* \
	\
	--enable-decoder=aac* \
	--enable-decoder=ac3 \
	--enable-decoder=alac \
	--enable-decoder=als \
	--enable-decoder=ape \
	--enable-decoder=atrac* \
	--enable-decoder=eac3 \
	--enable-decoder=flac \
	--enable-decoder=gsm* \
	--enable-decoder=mp1* \
	--enable-decoder=mp2* \
	--enable-decoder=mp3* \
	--enable-decoder=mpc* \
	--enable-decoder=opus \
	--enable-decoder=ra* \
	--enable-decoder=ralf \
	--enable-decoder=shorten \
	--enable-decoder=tak \
	--enable-decoder=tta \
	--enable-decoder=wavpack \
	--enable-decoder=wma* \
	--enable-decoder=pcm* \
	--enable-decoder=dsd* \
	--enable-decoder=dca \
	\
	--enable-decoder=ssa \
	--enable-decoder=ass \
	--enable-decoder=dvbsub \
	--enable-decoder=dvdsub \
	--enable-decoder=srt \
	--enable-decoder=stl \
	--enable-decoder=subrip \
	--enable-decoder=subviewer \
	--enable-decoder=subviewer1 \
	--enable-decoder=text \
	--enable-decoder=vplayer \
	--enable-decoder=webvtt \
	--enable-decoder=movtext \
	\
	--enable-demuxer=concat \
	--enable-demuxer=data \
	--enable-demuxer=flv \
	--enable-demuxer=hls \
	--enable-demuxer=latm \
	--enable-demuxer=live_flv \
	--enable-demuxer=loas \
	--enable-demuxer=m4v \
	--enable-demuxer=mov \
	--enable-demuxer=mpegps \
	--enable-demuxer=mpegts \
	--enable-demuxer=mpegvideo \
	--enable-demuxer=hevc \
	--enable-demuxer=rtsp \
	--enable-demuxer=mpeg4 \
	--enable-demuxer=avi \
	--enable-demuxer=av1 \
	--enable-demuxer=matroska \
	--enable-libxml2 \
	--enable-demuxer=dash \
	--enable-demuxer=webm_dash_manifest \
	--enable-muxer=webp \
	\
	--enable-demuxer=aac \
	--enable-demuxer=ac3 \
	--enable-demuxer=aiff \
	--enable-demuxer=ape \
	--enable-demuxer=asf \
	--enable-demuxer=au \
	--enable-demuxer=avi \
	--enable-demuxer=flac \
	--enable-demuxer=flv \
	--enable-demuxer=matroska \
	--enable-demuxer=mov \
	--enable-demuxer=m4v \
	--enable-demuxer=mp3 \
	--enable-demuxer=mpc* \
	--enable-demuxer=pcm* \
	--enable-demuxer=rm \
	--enable-demuxer=shorten \
	--enable-demuxer=tak \
	--enable-demuxer=tta \
	--enable-demuxer=wav \
	--enable-demuxer=wv \
	--enable-demuxer=xwma \
	--enable-demuxer=dsf \
	--enable-demuxer=truehd \
	--enable-demuxer=dts \
	--enable-demuxer=dtshd \
	\
	--enable-demuxer=ass \
	--enable-demuxer=srt \
	--enable-demuxer=stl \
	--enable-demuxer=webvtt \
	--enable-demuxer=subviewer \
	--enable-demuxer=subviewer1 \
	--enable-demuxer=vplayer \
	\
	--enable-parser=h263 \
	--enable-parser=h264 \
	--enable-parser=hevc \
	--enable-parser=mpeg4 \
	--enable-parser=mpeg4video \
	--enable-parser=mpegvideo \
	\
	--enable-parser=aac* \
	--enable-parser=ac3 \
	--enable-parser=cook \
	--enable-parser=dca \
	--enable-parser=flac \
	--enable-parser=gsm \
	--enable-parser=mpegaudio \
	--enable-parser=tak \
 	--enable-parser=dca \
	\
	--enable-filter=overlay \
	--enable-filter=equalizer \
	--enable-filter=aresample \
 	--enable-filter=dynaudnorm \
 	--enable-filter=loudnorm \
 	--enable-filter=alimiter \
	\
	--enable-protocol=async \
	--enable-protocol=cache \
	--enable-protocol=crypto \
	--enable-protocol=data \
	--enable-protocol=ffrtmphttp \
	--enable-protocol=file \
	--enable-protocol=ftp \
	--enable-protocol=hls \
	--enable-protocol=http \
	--enable-protocol=httpproxy \
	--enable-protocol=https \
	--enable-protocol=pipe \
	--enable-protocol=rtmp \
	--enable-protocol=rtmps \
	--enable-protocol=rtmpt \
	--enable-protocol=rtmpts \
	--enable-protocol=rtp \
	--enable-protocol=subfile \
	--enable-protocol=tcp \
	--enable-protocol=tls \
	--enable-protocol=srt \
	\
	--enable-encoder=png \
	--enable-encoder=libwebp \
	--enable-encoder=libwebp_anim \
	\
	--enable-network \
	$BVC2_FLAGS \

make -j$cores
make DESTDIR="$prefix_dir" install

ln -sf "$prefix_dir"/lib/libswresample.so "$native_dir"
ln -sf "$prefix_dir"/lib/libavutil.so "$native_dir"
ln -sf "$prefix_dir"/lib/libavcodec.so "$native_dir"
ln -sf "$prefix_dir"/lib/libavformat.so "$native_dir"
ln -sf "$prefix_dir"/lib/libswscale.so "$native_dir"
ln -sf "$prefix_dir"/lib/libavfilter.so "$native_dir"
ln -sf "$prefix_dir"/lib/libavdevice.so "$native_dir"

# ---- ByteVC2 demuxer 探针（仅 x86_64 轮）：用本架构 prefix 的静态
# libavformat 编 probe（含打补丁的 mov.c），native 运行验证 bv2C 子 box
# -> codec id 切换的实际行为；arm64 的 linker/解码层由真机验收覆盖 ----
if [[ "$ndk_triple" == "x86_64"* ]] && [ -f "$BVC2_DIR/bvc2_plain.mp4" ]; then
	# .pc 的 Libs 含 Android 专属库（-landroid/-lmediandk，configure --enable-jni
	# 引入），native Linux 无此库——生成空 stub 归档仅满足 -l 引用（探针代码
	# 不引 Android 符号，静态按需拉 .o 不会真用到）
	mkdir -p /tmp/bvc2stub && cd /tmp/bvc2stub
	for lib in android mediandk OpenSLES EGL; do ar rcs lib$lib.a 2>/dev/null; done
	cd - >/dev/null
	gcc -O1 "$BVC2_DIR/probe.c" -o /tmp/bvc2probe 		$(PKG_CONFIG_LIBDIR="$prefix_dir/lib/pkgconfig" pkg-config --static --cflags --libs libavformat libavcodec) 		-L/tmp/bvc2stub -lm -lpthread -lz || echo "PROBE 编译失败（继续构建不阻断）"
	[ -x /tmp/bvc2probe ] && /tmp/bvc2probe "$BVC2_DIR/bvc2_plain.mp4" || echo "PROBE 运行失败"
fi
