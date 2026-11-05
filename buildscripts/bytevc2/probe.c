/* ByteVC2 demuxer/desc 探针：验证两件事
 * ① mov.c 的 bv2C 子 box -> codec id 切换（demuxer 打开测试文件后流
 *    的 codec_id 应为 AV_CODEC_ID_BYTEVC2）；
 * ② avcodec_descriptor_get(BYTEVC2) 是否能查到描述条目（mpv 的
 *    mp_codec_from_av_codec_id 依赖它拿 codec 名——查不到 mpv 会按
 *    codec_tag fallback 成 hevc，即真机实锤的现象）。
 * 用法: probe <bvc2_plain.mp4>                                    */
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <stdio.h>

int main(int argc, char **argv)
{
    AVFormatContext *fc = NULL;
    int ret;

    if (argc < 2) {
        printf("PROBE usage: probe <file>\n");
        return 1;
    }
    /* desc 直查在前（不依赖文件） */
    const AVCodecDescriptor *desc =
        avcodec_descriptor_get(AV_CODEC_ID_BYTEVC2);
    printf("PROBE DESC ptr=%p name=%s\n",
           (void *)desc, desc ? desc->name : "NULL");
    const AVCodec *dec = avcodec_find_decoder(AV_CODEC_ID_BYTEVC2);
    printf("PROBE DECODER ptr=%p name=%s\n",
           (void *)dec, dec ? dec->name : "NULL");

    ret = avformat_open_input(&fc, argv[1], NULL, NULL);
    if (ret < 0) {
        printf("PROBE open failed: %d\n", ret);
        return 1;
    }
    ret = avformat_find_stream_info(fc, NULL);
    if (ret < 0) {
        printf("PROBE find_stream_info failed: %d\n", ret);
        return 1;
    }
    for (unsigned i = 0; i < fc->nb_streams; i++) {
        AVStream *st = fc->streams[i];
        printf("PROBE stream[%u] type=%d codec_id=%d codec_tag=%08X name=%s extradata_size=%d\n",
               i, st->codecpar->codec_type, st->codecpar->codec_id,
               st->codecpar->codec_tag,
               avcodec_get_name(st->codecpar->codec_id),
               st->codecpar->extradata_size);
    }
    printf("PROBE IDS bytevc2=%d hevc=%d\n",
           (int)AV_CODEC_ID_BYTEVC2, (int)AV_CODEC_ID_HEVC);
    avformat_close_input(&fc);
    return 0;
}
