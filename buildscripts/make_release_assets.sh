#!/bin/bash -e
# 生成 Release 资产：校验和（sha256 + md5）+ 构建信息（版本锚点从 depinfo.sh 读，
# 可复核）。供 .github/workflows/build.yaml 的 release 步骤调用。
#
# 用法：make_release_assets.sh <so 目录> [输出目录，缺省同 so 目录]

so_dir="${1:?用法: make_release_assets.sh <so 目录> [输出目录]}"
out_dir="${2:-$so_dir}"
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

. "$here/include/depinfo.sh"

cd "$so_dir"
ls libmpv-*.so >/dev/null # 无产物直接失败，别发布空 release
sha256sum libmpv-*.so > "$out_dir/SHA256SUMS"
md5sum libmpv-*.so > "$out_dir/MD5SUMS"

{
	echo "# libmpv Android 构建（DsPlayer 定制）"
	echo
	echo "- 构建时间（UTC）：$(date -u +'%Y-%m-%dT%H:%M:%SZ')"
	echo "- 提交：\`${GITHUB_SHA:-unknown}\`"
	echo "- ffmpeg \`${v_ffmpeg}\` ｜ mpv \`${v_mpv}\` ｜ libxml2 \`${v_libxml2}\`（DASH demuxer 依赖）"
	echo "- NDK \`${v_ndk}\` ｜ SDK platform \`${v_platform}\`"
	echo
	echo "## 产物"
	echo
	for f in libmpv-*.so; do
		printf -- '- `%s`（%s，sha256 %s…）\n' \
			"$f" "$(du -h "$f" | cut -f1)" "$(sha256sum "$f" | cut -c1-16)"
	done
	echo
	echo "## 用途"
	echo
	echo 'DsPlayer「MPV 内核插件」的 jniLibs 载体：`libmpv-<abi>.so` →'
	echo '`plugin_mpv/app/src/main/jniLibs/<abi>/libmpv.so`。'
	echo
	echo '含 `ff_dash_demuxer`（DASH/MPD 原生播放，依赖静态链接的 libxml2）——'
	echo '上游 libmpv 构建未编 libxml2 时该 demuxer 会被 ffmpeg 静默跳过。'
	echo
	echo '校验：`SHA256SUMS` / `MD5SUMS`（DsPlayer 插件市场用 md5 校验包）。'
} > "$out_dir/BUILD-INFO.md"

echo "===== release 资产（$out_dir）====="
ls -la "$out_dir"
