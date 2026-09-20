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
# prefix_name 兜底：本脚本以子进程运行（build.sh 用 `$BUILDSCRIPT build` 调用），
# 只能看到父进程 **export** 的变量。build.sh 历史上没 export prefix_name（本 fork
# 已补），拿不到就按 ndk_triple 反推——**不能放过空值**：NDK 的 android.toolchain.cmake
# 在 ANDROID_ABI 为空时默认 armeabi-v7a，会静默编出 ARM32 静态库，
# 直到 libmpv 链接期才报 "is incompatible with aarch64linux"（2026-09-21 CI 实锤）。
if [ -z "$prefix_name" ]; then
	case "$ndk_triple" in
		aarch64-*) prefix_name=arm64-v8a ;;
		arm-*)     prefix_name=armeabi-v7a ;;
		x86_64-*)  prefix_name=x86_64 ;;
		i686-*)    prefix_name=x86 ;;
		*) echo "无法从 ndk_triple='$ndk_triple' 推出 Android ABI"; exit 1 ;;
	esac
	echo "prefix_name 未导出，按 ndk_triple 推出: $prefix_name"
fi

# NDK 根定位：按可靠度依次探测
#   ① $ANDROID_HOME/ndk/$v_ndk（path.sh 的 PATH 也按这个版本拼，最优先）
#   ② $ANDROID_NDK_HOME（CI runner 常设；path.sh 只 unset 了 _ROOT 系列）
#   ③ $ANDROID_HOME/ndk/ 下任意版本目录（版本号漂移时的兜底）
#   ④ 从 clang 路径上溯 5 级（<ndk>/toolchains/llvm/prebuilt/<host>/bin/clang）
ndk_root=""
if [ -n "$ANDROID_HOME" ] && [ -d "$ANDROID_HOME/ndk/$v_ndk" ]; then
	ndk_root="$ANDROID_HOME/ndk/$v_ndk"
elif [ -n "$ANDROID_NDK_HOME" ] && [ -d "$ANDROID_NDK_HOME" ]; then
	ndk_root="$ANDROID_NDK_HOME"
elif [ -n "$ANDROID_HOME" ] && [ -d "$ANDROID_HOME/ndk" ]; then
	ndk_root="$(ls -d "$ANDROID_HOME"/ndk/*/ 2>/dev/null | sort -V | tail -1)"
	ndk_root="${ndk_root%/}"
fi
if [ -z "$ndk_root" ]; then
	ndk_root="$(cd "$(dirname "$(command -v clang)")/../../../../.." && pwd)"
fi
[ -d "$ndk_root" ] || { echo "找不到 NDK 根: $ndk_root"; exit 1; }

# 诊断：交叉编译是否真的生效，全看这几个值（2026-09-21 CI 实锤：libxml2 被
# 宿主 clang 编成 x86_64，链接期报 "is incompatible with aarch64linux"，
# 而日志里没有任何 NDK/Android 痕迹——必须先确认编译器和 toolchain 落点）
echo "=== libxml2 交叉编译环境诊断 ==="
echo "  ANDROID_HOME=$ANDROID_HOME"
echo "  v_ndk=$v_ndk    ndk_root=$ndk_root"
echo "  CC=$CC"
echo "  cmake: $(cmake --version 2>&1 | head -1)"
echo "  ndk 目录: $(ls -d "$ANDROID_HOME"/ndk/*/ 2>/dev/null | tr '\n' ' ')"
echo "  CC 版本: $($CC --version 2>&1 | head -1)"
echo "=== 诊断结束 ==="

# cmake 参数：优先 NDK 官方 toolchain 文件；文件不存在（新 NDK 版本可能移除）
# 则退回 CMake 内置 Android 支持（-DCMAKE_SYSTEM_NAME=Android + NDK 路径）。
# 两条路都显式钉 C 编译器 = $CC（NDK 的 aarch64-linux-android24-clang wrapper，
# wrapper 自带 --target 与 --sysroot，是 libwebp/dav1d 等依赖已验证可用的通路）。
toolchain="$ndk_root/build/cmake/android.toolchain.cmake"
if [ -f "$toolchain" ]; then
	echo "使用 NDK toolchain 文件: $toolchain"
	cmake_args=(-DCMAKE_TOOLCHAIN_FILE="$toolchain" -DANDROID_ABI="$prefix_name" -DANDROID_PLATFORM=android-24)
else
	echo "NDK toolchain 文件不存在，改用 CMake 内置 Android 支持"
	cmake_args=(-DCMAKE_SYSTEM_NAME=Android -DCMAKE_SYSTEM_VERSION=24
		-DCMAKE_ANDROID_ARCH_ABI="$prefix_name" -DCMAKE_ANDROID_NDK="$ndk_root")
fi

cmake .. \
	"${cmake_args[@]}" \
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

# 决定性事实：CMake 究竟选了哪个编译器 / 认为是哪个系统（唯一事实源是缓存）
cache="CMakeCache.txt"
echo "===== CMake 实际选择 ====="
for k in CMAKE_C_COMPILER CMAKE_SYSTEM_NAME CMAKE_ANDROID_ARCH_ABI CMAKE_TOOLCHAIN_FILE; do
	grep -E "^${k}(:FILEPATH)?=" "$cache" 2>/dev/null | sed 's/^/  /' || echo "  $k: (未设置)"
done
echo "=========================="

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
# 架构探测：`file` 对 .a 只报 "current ar archive"（无架构信息），抽一个成员看
# ELF 头才有效——静态库必须是目标 ABI（ARM aarch64 / x86-64），混错架构会在
# 后面 ffmpeg/libmpv 链接期才炸，报 "is incompatible with aarch64linux"。
ar_member=$("$AR" t "$prefix_dir/lib/libxml2.a" 2>/dev/null | head -1)
if [ -n "$ar_member" ]; then
	probe_dir=$(mktemp -d)
	(cd "$probe_dir" && "$AR" x "$prefix_dir/lib/libxml2.a" "$ar_member" 2>/dev/null)
	echo "  静态库架构: $(file -b "$probe_dir/$ar_member" 2>&1)"
	rm -rf "$probe_dir"
fi
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
