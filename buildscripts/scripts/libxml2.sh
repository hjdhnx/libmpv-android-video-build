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

# libxml2 静态库（ffmpeg dash demuxer 的硬依赖：dash_demuxer_deps="libxml2"，
# 见 ffmpeg configure 的 require_pkg_config libxml2 libxml-2.0 ...）。
#
# 用 NDK 官方 CMake toolchain（git 源码无现成 configure；autogen 需完整
# autotools 链，而 CMakeLists 是上游一等公民、交叉编译更省事）。
# 关闭非必需部件：Python 绑定 / lzma / zlib / iconv / HTTP / FTP —— DASH 只需
# XML 解析与 MPD 读入（减体积，也减少交叉编译依赖面）。
#
# ⚠ 安装走 buildscripts 统一约定：**不设 CMAKE_INSTALL_PREFIX**（保持默认
# /usr/local）+ `make DESTDIR="$prefix_dir" install`。build.sh 的 setup_prefix()
# 已把 $prefix_dir/usr 与 $prefix_dir/local 都 ln -s 到 $prefix_dir 自身（扁平化），
# 所以装到 <prefix_dir>/usr/local/lib 实际落在 <prefix_dir>/lib。
# .pc 里于是写 prefix=/usr/local，读时由 PKG_CONFIG_SYSROOT_DIR=$prefix_dir
# 前置成 <prefix_dir>/usr/local/... → 解析回真实路径。
# **勿改用 -DCMAKE_INSTALL_PREFIX=$prefix_dir 裸 install**：.pc 会写成绝对
# prefix=<prefix_dir>，与 PKG_CONFIG_SYSROOT_DIR 再叠加即双重前缀
# （实测 -I<prefix_dir><prefix_dir>/include/libxml2，路径不存在 → ffmpeg 报
# "libxml-2.0 not found using pkg-config"）。
#
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

# .pc 的 Cflags 补一条 -I${includedir}：libxml2 官方 .pc 的布局约定是
# `#include <libxml/parser.h>` + `-I${includedir}/libxml2`，而 ffmpeg 的
# dash 检测写的是 homebrew 风格 `#include <libxml2/libxml/xmlversion.h>`
# （require_pkg_config libxml2 libxml-2.0 libxml2/libxml/xmlversion.h xmlCheckVersion）
# —— 只给 -I${includedir}/libxml2 时编译器会去找 include/libxml2/libxml2/...。
# 两条 -I 都留着（两种 include 形态都能解析），这也是发行版打包的常规做法。
pc="$prefix_dir/lib/pkgconfig/libxml-2.0.pc"
if [ -f "$pc" ]; then
	grep -v '^Cflags:' "$pc" > "$pc.new" && mv "$pc.new" "$pc"
	printf 'Cflags: -I${includedir}/libxml2 -I${includedir} -DLIBXML_STATIC\n' >> "$pc"
fi

# 诊断：确认头文件/静态库/.pc 落点与 PKG_CONFIG_SYSROOT_DIR 展开后一致
echo "=== libxml2 安装诊断 ==="
echo "prefix_dir=$prefix_dir"
echo "PKG_CONFIG_LIBDIR=$PKG_CONFIG_LIBDIR"
find "$prefix_dir" -name 'libxml-2.0.pc' 2>/dev/null | sed 's/^/  .pc → /'
ls -la "$prefix_dir/include/libxml2/libxml/xmlversion.h" 2>/dev/null | sed 's/^/  header → /' || echo "  header → MISSING"
ls -la "$prefix_dir/lib/libxml2.a" 2>/dev/null | sed 's/^/  lib → /' || echo "  lib → MISSING"
pkg-config --exists libxml-2.0 && echo "  pkg-config: OK" || echo "  pkg-config: NOT FOUND"
pkg-config --modversion libxml-2.0 2>&1 | sed 's/^/  version: /'
echo "  cflags: $(pkg-config --cflags libxml-2.0 2>&1)"
echo "  libs:   $(pkg-config --static --libs libxml-2.0 2>&1)"

# 决定性自检：复刻 ffmpeg 的 require_pkg_config 检测（include + 链接
# xmlCheckVersion），在 libxml2 这一步就判死，别等 ffmpeg configure 报
# 那句没有上下文的 "libxml-2.0 not found using pkg-config"。
# 变体 ② 带 --extra-cflags 的 -I$prefix_dir/include，与 ffmpeg 实际编译一致=闸门；
# 变体 ① 纯 pkg-config 输出=参考值。
tmpc="${TMPDIR:-/tmp}/libxml2_check.c"
cat > "$tmpc" <<'EOF'
#include <libxml2/libxml/xmlversion.h>
int main(void) { xmlCheckVersion(LIBXML_VERSION); return 0; }
EOF
if $CC "$tmpc" -I"$prefix_dir/include" $(pkg-config --cflags --libs --static libxml-2.0) -o "${TMPDIR:-/tmp}/libxml2_check2" 2>"${TMPDIR:-/tmp}/libxml2_check2.err"; then
	echo "  编译自检② (ffmpeg 同款 flags): OK"
else
	echo "  编译自检② (ffmpeg 同款 flags): FAIL"
	cat "${TMPDIR:-/tmp}/libxml2_check2.err"
	echo "=== 诊断结束（自检未过，终止构建）==="
	exit 1
fi
$CC "$tmpc" $(pkg-config --cflags --libs --static libxml-2.0) -o "${TMPDIR:-/tmp}/libxml2_check1" 2>/dev/null \
	&& echo "  编译自检① (纯 pkg-config): OK" || echo "  编译自检① (纯 pkg-config): FAIL(仅参考)"
echo "=== 诊断结束 ==="
