#!/usr/bin/env python3
"""
apply_ffmpeg_patch.py - 把 ByteVC2 解码器接入 FFmpeg 源码树

用法: python3 apply_ffmpeg_patch.py /path/to/ffmpeg

对 FFmpeg 树做以下修改 (全部为锚点插入, 幂等: 重复执行自动跳过):
  1. libavcodec/bytevc2dec.c / bytevc2_api.h / bytevc2_nal.c / bytevc2_nal.h  拷入
  2. libavcodec/codec_id.h      新增 AV_CODEC_ID_BYTEVC2 (视频 ID 段末尾)
  3. libavcodec/codec_desc.c    新增解码器描述条目
  4. libavcodec/allcodecs.c     新增 extern 声明
     (注意: codec_list.c 与 configure 的 DECODER_LIST 均由 extern 声明
      构建期自动生成, 无需手改; 适用于 FFmpeg >= 4.x 的生成机制)
  5. libavcodec/Makefile        新增 OBJS-$(CONFIG_BYTEVC2_DECODER)
  6. libavformat/isom_tags.c    ff_codec_movvideo_tags 增加 'bv2C' fourcc
     (FFmpeg <= 6.0 该表在 libavformat/isom.c, 脚本自动探测)
  7. libavformat/mov.c          mov_default_parse_table 增加 'bv2C' extradata 读取
"""
import os
import re
import shutil
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = HERE  # 源文件与脚本同目录（构建仓布局，与上游补丁包 libavcodec-src/ 等价）


def read(p):
    with open(p, "r", encoding="utf-8") as f:
        return f.read()


def write(p, s):
    with open(p, "w", encoding="utf-8") as f:
        f.write(s)


def insert_after(text, anchor_regex, insertion, label):
    if insertion.strip() and insertion.strip() in text:
        print(f"  [skip] {label} (已存在)")
        return text
    it = list(re.finditer(anchor_regex, text, re.M))
    if not it:
        raise RuntimeError(f"未找到锚点: {label} ({anchor_regex})")
    m = it[-1]
    end = text.index("\n", m.end()) + 1
    print(f"  [ok]   {label}")
    return text[:end] + insertion + text[end:]


def insert_before(text, anchor_regex, insertion, label, last=False):
    if insertion.strip() and insertion.strip() in text:
        print(f"  [skip] {label} (已存在)")
        return text
    it = list(re.finditer(anchor_regex, text, re.M))
    if not it:
        raise RuntimeError(f"未找到锚点: {label} ({anchor_regex})")
    m = it[-1] if last else it[0]
    print(f"  [ok]   {label}")
    return text[:m.start()] + insertion + text[m.start():]


def apply(tree):
    # ---- 1. 拷入源文件 ----
    dst_dir = os.path.join(tree, "libavcodec")
    for fn in ("bytevc2dec.c", "bytevc2_api.h", "bytevc2_nal.c", "bytevc2_nal.h"):
        shutil.copy2(os.path.join(SRC, fn), os.path.join(dst_dir, fn))
        print(f"  [ok]   libavcodec/{fn}")

    # ---- 2. codec_id.h ----
    # ⚠️ 显式取 WRAPPED_AVFRAME(0x21001) 之后的私有段大值: FFmpeg >= 7 的
    # avcodec_descriptor_get 用 bsearch 按 id 升序查 codec_desc 表, 插表尾
    # 的条目必须是大值 (若取 FIRST_AUDIO 前的连续小值 274, 表尾插入即破坏
    # 排序, mpv 按 desc 查 codec 名必失败而 fallback, CI 真机实锤)
    p = os.path.join(dst_dir, "codec_id.h")
    t = read(p)
    t = insert_before(
        t, r"^\s*AV_CODEC_ID_FIRST_AUDIO\b",
        "    AV_CODEC_ID_BYTEVC2 = 0x21004,\n"
        "    /* 私有解码器 (mp4 fourcc 'bv2C'), WRAPPED_AVFRAME(0x21001) 之后\n"
        "     * 的私有空位: desc 表按 id 升序 + bsearch, 见 codec_desc.c 插位 */\n",
        "codec_id.h: AV_CODEC_ID_BYTEVC2")
    write(p, t)

    # ---- 3. codec_desc.c ----
    # 插表尾 }; 前: BYTEVC2 = 0x21002 (见 codec_id.h) > 表尾最大条目
    # WRAPPED_AVFRAME(0x21001), 升序保持, 9.0 的 bsearch 可查。
    # 幂等: bytevc2 名字已存在即跳过 (insert_before 内部也兜一层)。
    p = os.path.join(dst_dir, "codec_desc.c")
    t = read(p)
    desc = ("    {\n"
            "        .id        = AV_CODEC_ID_BYTEVC2,\n"
            "        .type      = AVMEDIA_TYPE_VIDEO,\n"
            "        .name      = \"bytevc2\",\n"
            "        .long_name = NULL_IF_CONFIG_SMALL(\"ByteVC2 (software)\"),\n"
            "        .props     = AV_CODEC_PROP_LOSSY,\n"
            "    },\n")
    t = insert_before(t, r"^\};", desc,
                      "codec_desc.c: bytevc2 条目(表尾, 0x21004 排序正确)")
    write(p, t)

    # ---- 4. allcodecs.c: 仅 extern 声明 (codec_list/DECODER_LIST 自动生成) ----
    p = os.path.join(dst_dir, "allcodecs.c")
    t = read(p)
    style = "FFCodec" if "codec_internal.h" in t else "AVCodec"
    t = insert_after(
        t, r"^extern const %s ff_[a-z0-9_]+_decoder;" % style,
        f"extern const {style} ff_bytevc2_decoder;\n",
        f"allcodecs.c: extern ({style})")
    write(p, t)

    # ---- 5. libavcodec/Makefile ----
    p = os.path.join(dst_dir, "Makefile")
    t = read(p)
    t = insert_before(
        t, r"^OBJS-\$\(CONFIG_ZMBV_DECODER\)",
        "OBJS-$(CONFIG_BYTEVC2_DECODER)      += bytevc2dec.o bytevc2_nal.o\n",
        "libavcodec/Makefile: OBJS 条目")
    write(p, t)

    # ---- 6. mov fourcc 表 (FFmpeg 6.1+ 在 isom_tags.c, 旧版在 isom.c) ----
    tag_file = os.path.join(tree, "libavformat", "isom_tags.c")
    if not os.path.exists(tag_file):
        tag_file = os.path.join(tree, "libavformat", "isom.c")
    t = read(tag_file)
    t = insert_before(
        t, r"^\s*\{\s*AV_CODEC_ID_RAWVIDEO,\s*MKTAG\('r', 'a', 'w', ' '\)\s*\}",
        "    { AV_CODEC_ID_BYTEVC2, MKTAG('b', 'v', '2', 'C') },\n",
        f"{os.path.basename(tag_file)}: 'bv2C' fourcc")
    write(tag_file, t)

    # ---- 7. mov.c extradata 解析 ----
    p = os.path.join(tree, "libavformat", "mov.c")
    t = read(p)
    fn = """
static int mov_read_bv2c(MOVContext *c, AVIOContext *pb, MOVAtom atom)
{
    AVStream *st;
    int ret;

    if (c->fc->nb_streams < 1)
        return 0;
    st = c->fc->streams[c->fc->nb_streams - 1];

    if ((uint64_t)atom.size > (1 << 30))
        return AVERROR_INVALIDDATA;

    /* bv2C 参数集容器整体作为 extradata, 由 bytevc2 解码器转 Annex-B */
    ret = ff_get_extradata(c->fc, st->codecpar, pb, atom.size);
    if (ret < 0)
        return ret;

    /* ByteVC2 码流在实际生态中常伪装于 hvc1/hev1 sample entry 内
     * (字节系播放器在 HEVC 解码器路径上替换实现)。检测到 bv2C
     * 参数集容器即切换 codec id, 使播放器自动选用 bytevc2 解码器 */
    if (st->codecpar->codec_id == AV_CODEC_ID_HEVC)
        st->codecpar->codec_id = AV_CODEC_ID_BYTEVC2;

    return 0;
}

"""
    if "mov_read_bv2c" not in t:
        anchor = "static int mov_read_glbl("
        idx = t.index(anchor)
        t = t[:idx] + fn + t[idx:]
        print("  [ok]   mov.c: mov_read_bv2c 函数")
    else:
        print("  [skip] mov.c: mov_read_bv2c 函数 (已存在)")

    t = insert_before(
        t, r"^\{\s*MKTAG\('a','v','1','C'\),\s*mov_read_glbl\s*\},",
        "{ MKTAG('b','v','2','C'), mov_read_bv2c },\n",
        "mov.c: parse table 注册")
    write(p, t)

    print("补丁应用完成。")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(1)
    tree = sys.argv[1]
    if not os.path.isdir(os.path.join(tree, "libavcodec")):
        print(f"错误: {tree} 不是 FFmpeg 源码树")
        sys.exit(1)
    print(f"应用 ByteVC2 补丁到: {tree}")
    apply(tree)


if __name__ == "__main__":
    main()
