# libmpv-android-video-build（DsPlayer 定制分支）

基于 [bggRGjQaUbCoE/libmpv-android-video-build](https://github.com/bggRGjQaUbCoE/libmpv-android-video-build) 的定制 fork，
由 [DsPlayer](https://github.com/hjdhnx/DsPlayer) 维护。**产出 = Android 各架构的 `libmpv.so`**，
供 DsPlayer 的「MPV 内核插件」跨包 dlopen 使用。

## 这个仓库做什么

交叉编译 Android 平台的 **libmpv**（含 FFmpeg 及其依赖），产物是单个 `libmpv.so`：

```
buildscripts/build.sh --arch <架构> mpv
  → 下载并编译 mbedtls / dav1d / libwebp / libxml2（FFmpeg 依赖）
  → 编译 FFmpeg（9.0.1，静态库）
  → 编译 mpv（0.41.0，libmpv 形态）
  → prefix/<abi>/lib/libmpv.so
```

DsPlayer 侧的用法：`libmpv-<abi>.so` → `plugin_mpv/app/src/main/jniLibs/<abi>/libmpv.so`。

## 相比上游的变化（本 fork 的增量）

### 1. 启用 DASH（MPD）demuxer

**上游的 `libmpv.so` 不支持 DASH 播放**，根因在 FFmpeg 的依赖检查：

```sh
# FFmpeg configure
dash_demuxer_deps="libxml2"      # ← 硬依赖
enabled libxml2 && require_pkg_config libxml2 libxml-2.0 libxml2/libxml/xmlversion.h xmlCheckVersion
```

上游未编 `libxml2`，于是 `--enable-demuxer=dash` 被 configure **静默跳过**
（不报错、不提示），产物里 `ff_hls_demuxer` 等符号齐全而 `ff_dash_demuxer` 缺失。
运行时表现：`Unknown lavf format dash` + `cplayer: Failed to recognize file format`
（20ms 内失败——选不中 demuxer 就放弃）。

本 fork 补齐依赖链：

- 新增 `buildscripts/scripts/libxml2.sh`：NDK CMake 交叉编译 libxml2 静态库
  （关闭 Python/lzma/zlib/iconv/HTTP/FTP，只留 DASH 需要的 XML 解析与 MPD 读入）；
- `include/depinfo.sh` + `download-deps.sh`：注册 libxml2（`v_libxml2=2.13.5`），
  加进 `dep_ffmpeg`；
- `flavors/default.sh`：`--enable-demuxer=dash` 前加 `--enable-libxml2`。

**验证**（符号级，免真机）：`ff_dash_demuxer` 符号存在、demuxer 总数 72 → 73、
字符串表出现 `Representation`/`SegmentTemplate`/`AdaptationSet`/`xmlReadMemory`、
动态依赖表无新增（libxml2 静态吸收）、SONAME 不变。

DsPlayer 实测：哔哩影视[官](DS) 源的 DASH 流，MPV 内核首帧 **39ms**
（此前后端不支持时只能降级到 Exo，首帧 851ms）。

### 2. 逐架构构建 + 一键 Release 发布

上游 CI 只出完整 bundle（含 Flutter helper 段），且不发布 `.so`。本 fork：

- **`arches` 输入**（workflow_dispatch）：`armv7l` / `arm64` / `x86` / `x86_64`
  空格分隔，缺省全编。构建脚本口径是 `armv7l` 等，**产物按 Android ABI 名命名**
  （`libmpv-armeabi-v7a.so` 等），可直接落 `jniLibs/<abi>/`；
- **跳过 helper/event_loop 的 Flutter 段**（DsPlayer 只要 libmpv.so），
  省一半以上构建时间；
- **`release` 输入**：勾选即创建 Release，资产 = 全部 `.so` + `SHA256SUMS` +
  `MD5SUMS` + `BUILD-INFO.md`（构建时间/提交/依赖版本锚点，版本号从
  `depinfo.sh` 读，可复核）。tag 缺省 `build-<日期>-<短提交>`，可经
  `releaseTag` 指定；
- 构建段末尾**自校验**产物：架构（`llvm-readelf` 看 Machine/Class）、
  `ff_dash_demuxer` 符号、demuxer 总数、`NEEDED` 列表。

### 3. 交叉编译正确性修复

- **`build.sh` 的 `prefix_name` 补 `export`**：依赖脚本以子进程运行
  （`$BUILDSCRIPT build`），只能看到 export 过的变量。漏了则 `$prefix_name`
  为空 → `-DANDROID_ABI=` 空值 → NDK toolchain 静默回落默认 ABI `armeabi-v7a`
  → 编出 ARM32 静态库，**直到 libmpv 链接期才报**
  `is incompatible with aarch64linux`。（libwebp 等不吃 `prefix_name`、只靠已
  导出的 `CC`，所以一直是好的——这种"部分依赖正常"的现象极易误判成环境问题。）
- **libxml2 安装走 buildscripts 统一约定**：不设 `CMAKE_INSTALL_PREFIX`
  （保持 `/usr/local`）+ `make DESTDIR="$prefix_dir" install`。`setup_prefix()`
  已把 `$prefix_dir/usr`、`$prefix_dir/local` 都 `ln -s` 到 `$prefix_dir` 自身
  （扁平化 `/usr/local -> /`），`.pc` 写 `prefix=/usr/local`，读时由
  `PKG_CONFIG_SYSROOT_DIR` 前置回真实路径。**反向做法必挂**：钉绝对
  `CMAKE_INSTALL_PREFIX=$prefix_dir` 会让 `.pc` 写成绝对 prefix，与 sysroot
  叠加生成 `-I<prefix_dir><prefix_dir>/include/libxml2`（路径不存在 →
  FFmpeg 报 `libxml-2.0 not found using pkg-config`）。
- **`.pc` 的 Cflags 补 `-I${includedir}`**：FFmpeg 的检测头是 homebrew 风格
  `<libxml2/libxml/xmlversion.h>`，libxml2 官方 `.pc` 只给
  `-I${includedir}/libxml2`，缺这条会找成 `include/libxml2/libxml2/...`。
- **每架构构建前清 `prefix/<abi>`**：否则上一架构留下的 `libxml2.a` 会被链接
  进去，且要到链接期才暴露。
- 构建失败时自动 dump 诊断（`.pc` 内容 / `pkg-config` 完整输出 /
  FFmpeg `config.log` 的 libxml 段），并在 libxml2 阶段加**决定性编译自检**
  （复刻 FFmpeg 的 `require_pkg_config` 检测：include + 链接 `xmlCheckVersion`），
  失败即在该步终止，不必等 FFmpeg 报没有上下文的错。

## 使用

### 方式一：直接用已发布的 .so（推荐）

到 [Releases](https://github.com/hjdhnx/libmpv-android-video-build/releases) 下载：

```
libmpv-arm64-v8a.so      → jniLibs/arm64-v8a/libmpv.so
libmpv-armeabi-v7a.so    → jniLibs/armeabi-v7a/libmpv.so
libmpv-x86.so            → jniLibs/x86/libmpv.so
libmpv-x86_64.so         → jniLibs/x86_64/libmpv.so
SHA256SUMS / MD5SUMS     # 校验
BUILD-INFO.md            # 构建信息（依赖版本锚点）
```

### 方式二：自己触发 CI

Actions → `Build libmpv-android` → Run workflow，填：

| 输入 | 说明 |
|---|---|
| `arches` | 要构建的架构，空格分隔（缺省 `armv7l arm64 x86 x86_64`） |
| `release` | 勾选 = 构建完创建 Release 并发布 `.so` |
| `releaseTag` | Release tag（留空 = `build-<日期>-<短提交>`） |

产物同时以 artifact（`libmpv-so`）形式附在 run 页。

### 方式三：本地构建（不推荐）

需要 Linux 环境 + 完整 Android SDK/NDK + autotools/meson/cmake 工具链，
本机搭这套环境通常要数小时与数 GB 空间。**多架构编 so 优先用 CI**
（见下方"设计取舍"）。

```sh
cd buildscripts
./download.sh && ./patch.sh
cp flavors/default.sh scripts/ffmpeg.sh
./build.sh --arch arm64 mpv        # 架构：armv7l / arm64 / x86 / x86_64
# 产物：prefix/arm64-v8a/lib/libmpv.so
```

## 依赖版本（锚点，见 `buildscripts/include/depinfo.sh`）

| 组件 | 版本 |
|---|---|
| FFmpeg | 9.0.1 |
| mpv | 0.41.0 |
| libxml2 | 2.13.5（DASH demuxer 依赖） |
| libplacebo | 7.360.1 |
| libass / harfbuzz / fribidi / freetype | 0.17.5 / 14.3.1 / 1.0.16 / 2-14-3 |
| dav1d / mbedtls / libwebp / libvpx | 1.5.4 / 3.6.5 / 1.6.0 / 1.16 |
| NDK | 29.0.14206865 |
| SDK platform | android-36 |

## 设计取舍

- **只出 `libmpv.so`，不出完整 bundle**：上游产物含 Flutter 侧 helper
  （`libmediakitandroidhelper.so`、`libmedia_kit_native_event_loop.so`）与
  jar；DsPlayer 的插件形态里这层由插件工程自带（helper 实际由本体打包，
  因为 JNI 库无法跨包 dlopen），故 CI 只取 libmpv.so 以省时间。
- **CI 优先而非本地构建**：libmpv 的交叉编译链（NDK + meson + cmake +
  autotools + Python + 各依赖源码）在本地搭建成本极高，且极易踩环境坑
  （JDK/镜像/缓存/NDK 版本/工具链路径）。GitHub Actions 上是干净
  ubuntu-22.04 + 预装 SDK/NDK，构建稳定可复现，还自带 Release 发布。
  单个架构约 3–5 分钟，四架构全量约 15 分钟。
- **不编 `x86` 之外的冷门架构**（如 riscv64）：Android 设备生态无需求。

## 与 DsPlayer 主仓的关系

- 本 fork 的产物消费方：DsPlayer `plugin_mpv/`（MPV 内核插件）；
- 构建陷阱与验收判据的完整记录：DsPlayer 仓库
  `docs/plugin/PLUGIN-DESIGN.md` §七（7.1 DASH / 7.2 多架构与 Release /
  7.3 按 ABI 拆包配方）；
- 上游仓库：[bggRGjQaUbCoE/libmpv-android-video-build](https://github.com/bggRGjQaUbCoE/libmpv-android-video-build)。

## 许可

继承上游与各依赖组件的许可（FFmpeg LGPL/GPL、mpv GPL/LGPL、libxml2 MIT 等）。
本 fork 的 `flavors/default.sh` 维持上游的 `--disable-gpl --enable-version3`
配置（LGPL v3）。
