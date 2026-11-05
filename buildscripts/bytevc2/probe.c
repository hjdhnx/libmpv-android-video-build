/* ByteVC2 demuxer 探针：验证 mov.c 的 bv2C 子 box -> codec id 切换
 * 在当前 FFmpeg 树的实际行为（CI 构建后 x86_64 native 编译运行，
 * 输出 PROBE 行给日志）。demuxer 层验证不需要解码器本体。
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
    /* BYTEVC2 的 AVCodecID 数值（补丁插在视频段末尾, 官方 LAST_VIDEO 与
     * FIRST_AUDIO 之间）; demuxer 若正确切换, 视频流 name 应为 bytevc2 */
    printf("PROBE AV_CODEC_ID_BYTEVC2=%d hevc=%d\n",
           AV_CODEC_ID_BYTEVC2, AV_CODEC_ID_HEVC);
    avformat_close_input(&fc);
    return 0;
}
