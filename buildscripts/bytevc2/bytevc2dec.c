/*
 * ByteVC2 软件解码器 - FFmpeg libavcodec 适配层
 *
 * 供 mpv 等基于 libavcodec 的播放器自动识别解码 ByteVC2 码流
 * (mp4 sample entry 'bv2C', extradata 为 bv2C 参数集容器)。
 *
 * 调用契约来自对 libttmplayer.so (vc2_dec.cpp) 交叉引用的逆向恢复,
 * 与 ExoPlayer 扩展 (bytevc2_jni.c) 同源, 并经 qemu-aarch64 驱动真实
 * libbyteVC2dec.so 0.9.7.1 实测校准 (1826 样本 / 1814 帧逐字节对拍):
 *
 *   - 输入为 Annex-B; mp4 长度前缀 sample 先转 Annex-B;
 *   - 码流内与 csd 重复的参数 NAL 必须剥离, 否则解码器报
 *     "The first frame is not I frame" 并丢弃整个 AU;
 *   - byteVC2dec_decode(ctx,pkt,frame) 同步直出: 返回 0 后读 frame 的
 *     "got_frame"/"width"/"height"/"linesize"/"data" (data[2]==NULL -> NV12);
 *   - pts 契约: 上游喂显示时间戳 (mov demuxer 的 pkt->pts 即 dts+ctts
 *     合成时间) 时解码器直出显示序, 无需重排缓冲;
 *   - 帧拷贝后立即 byteVC2dec_return_frame (内部环形缓冲仅 5 槽)。
 *
 * 目标 FFmpeg >= 6.1 (FFCodec/.p 初始化风格; FFmpeg 4.x/5.x 改法见 README)。
 */
#include "libavutil/imgutils.h"
#include "libavutil/mem.h"
#include "avcodec.h"
#include "codec_internal.h"
#include "decode.h"

#include "bytevc2_api.h"
#include "bytevc2_nal.h"

#include <string.h>

#define MAX_CSD_NALS 32
#define MAX_REORDER_NOTE "上游须喂显示序 pts (mov demuxer 默认如此)"

typedef struct ByteVc2DecContext {
  byteVC2dec_ctx    *ctx;
  byteVC2dec_packet *pkt;
  byteVC2dec_frame  *frame;
  byteVC2dec_param  *param;      /* 保留供 flush 后 reconfig */
  /* Annex-B 转换暂存区 */
  uint8_t *convBuf;
  int      convCap;
  /* csd 参数 NAL 列表 (剥离 sample 中重复的参数 NAL) */
  uint8_t *csdNal[MAX_CSD_NALS];
  int      csdNalLen[MAX_CSD_NALS];
  int      csdNalCount;
} ByteVc2DecContext;

static av_cold int csd_clear(ByteVc2DecContext *s)
{
  int i;
  for (i = 0; i < s->csdNalCount; i++) {
    av_freep(&s->csdNal[i]);
    s->csdNalLen[i] = 0;
  }
  s->csdNalCount = 0;
  return 0;
}

static av_cold int ensure_conv(ByteVc2DecContext *s, int cap)
{
  uint8_t *p;
  if (s->convCap >= cap)
    return 1;
  p = av_realloc(s->convBuf, cap);
  if (!p)
    return 0;
  s->convBuf = p;
  s->convCap = cap;
  return 1;
}

/* 从 extradata (bv2C 容器或 Annex-B) 记录全部参数 NAL */
static av_cold int csd_store(ByteVc2DecContext *s, const uint8_t *src, int len)
{
  int pos = 0;
  csd_clear(s);

  if (len > 4 && src[0] == 0x01 && !bytevc2_is_annexb(src, len)) {
    /* bv2C v1 容器: 19B 头 + [type1B][count2B] + count×[len2B+NAL] */
    int q = 19, ok = 1;
    while (ok && q + 3 <= len && s->csdNalCount < MAX_CSD_NALS) {
      int cnt = (src[q + 1] << 8) | src[q + 2];
      q += 3;
      if (cnt <= 0 || cnt > 64) { ok = 0; break; }
      while (cnt-- > 0 && q + 2 <= len) {
        int l = (src[q] << 8) | src[q + 1];
        q += 2;
        if (l <= 0 || q + l > len) { ok = 0; break; }
        s->csdNal[s->csdNalCount] = av_malloc(l);
        if (!s->csdNal[s->csdNalCount]) { ok = 0; break; }
        memcpy(s->csdNal[s->csdNalCount], src + q, l);
        s->csdNalLen[s->csdNalCount] = l;
        s->csdNalCount++;
        q += l;
      }
    }
    return ok ? s->csdNalCount : -1;
  }

  /* Annex-B: 起始码扫描 (回退上限 3 字节) */
  while (pos + 4 <= len && s->csdNalCount < MAX_CSD_NALS) {
    const uint8_t *p = src + pos;
    int hdrLen, nalLen, q, rb;
    if (p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) hdrLen = 4;
    else if (p[0] == 0 && p[1] == 0 && p[2] == 1)         hdrLen = 3;
    else break;
    nalLen = 0;
    q = pos + hdrLen;
    while (q + 3 <= len) {
      const uint8_t *r = src + q;
      if (r[0] == 0 && r[1] == 0 && r[2] == 1) break;
      q++;
    }
    if (q + 3 > len) q = len;
    else {
      rb = 0;
      while (q > pos + hdrLen && rb < 3 && src[q - 1] == 0) { q--; rb++; }
    }
    nalLen = q - (pos + hdrLen);
    if (nalLen <= 0) break;
    s->csdNal[s->csdNalCount] = av_malloc(nalLen);
    if (!s->csdNal[s->csdNalCount]) break;
    memcpy(s->csdNal[s->csdNalCount], src + pos + hdrLen, nalLen);
    s->csdNalLen[s->csdNalCount] = nalLen;
    s->csdNalCount++;
    pos += hdrLen + nalLen;
  }
  return s->csdNalCount;
}

/* 长度前缀 -> Annex-B 并剥离 csd 重复参数 NAL。
 * 优先按 4 字节长度前缀解析 (mp4 默认); 前缀解析失败则按 Annex-B 透传。 */
static int convert_sample(ByteVc2DecContext *s, const uint8_t *data, int len)
{
  static const uint8_t sc[4] = { 0, 0, 0, 1 };
  int pos = 0, written = 0, lengthSize = 4;

  if (lengthSize >= 1 && lengthSize <= 4) {
    int ok = 1;
    while (pos + lengthSize <= len) {
      int32_t nalLen = 0;
      int i, dup = 0;
      for (i = 0; i < lengthSize; i++)
        nalLen = (nalLen << 8) | data[pos + i];
      pos += lengthSize;
      if (nalLen <= 0 || pos + nalLen > len) { ok = 0; break; }
      for (i = 0; i < s->csdNalCount; i++) {
        if (s->csdNalLen[i] == nalLen &&
            memcmp(data + pos, s->csdNal[i], nalLen) == 0) { dup = 1; break; }
      }
      if (!dup) {
        if (written + 4 + nalLen > s->convCap) { ok = 0; break; }
        memcpy(s->convBuf + written, sc, 4);
        memcpy(s->convBuf + written + 4, data + pos, nalLen);
        written += 4 + nalLen;
      }
      pos += nalLen;
    }
    if (ok && pos == len)
      return written;
  }
  /* 兜底: 已是 Annex-B 则原样透传 */
  if (bytevc2_is_annexb(data, len)) {
    if (len > s->convCap)
      return -1;
    memcpy(s->convBuf, data, len);
    return len;
  }
  return -1;
}

static av_cold int bytevc2_init(AVCodecContext *avctx)
{
  ByteVc2DecContext *s = avctx->priv_data;
  char ver[16] = { 0 };
  int threads = avctx->thread_count > 0 ? avctx->thread_count : 8;

  s->param = byteVC2dec_get_default_param();
  if (!s->param) {
    av_log(avctx, AV_LOG_ERROR, "byteVC2dec: get_default_param failed\n");
    return AVERROR_EXTERNAL;
  }
  byteVC2dec_set_param_integer_info(s->param, "threads", threads);
  byteVC2dec_set_param_integer_info(s->param, "max_threads", threads);
  byteVC2dec_set_param_integer_info(s->param, "wpp_mode", 1);
  if (avctx->skip_frame >= AVDISCARD_NONKEY)
    byteVC2dec_set_param_integer_info(s->param, "low_latency", 1);
  /* CPU YUV 输出 (不走 AHardwareBuffer 直出) */
  byteVC2dec_set_param_integer_info(s->param, "hardware_buffer", 0);

  s->ctx = byteVC2dec_create(s->param);
  if (!s->ctx) {
    av_log(avctx, AV_LOG_ERROR, "byteVC2dec: create failed\n");
    byteVC2dec_release_param(s->param);
    s->param = NULL;
    return AVERROR_EXTERNAL;
  }
  s->pkt   = byteVC2dec_alloc_packet();
  s->frame = byteVC2dec_alloc_frame();
  if (!s->pkt || !s->frame) {
    av_log(avctx, AV_LOG_ERROR, "byteVC2dec: alloc packet/frame failed\n");
    return AVERROR_EXTERNAL;
  }
  byteVC2dec_get_version(ver);
  av_log(avctx, AV_LOG_VERBOSE, "byteVC2dec created, version=%s, threads=%d\n",
         ver, threads);

  /* extradata (mov demuxer 经 'bv2C' box 传入的参数集容器):
   * 记录参数 NAL 供剥离, 转 Annex-B 后以 pts=0 送入 */
  if (avctx->extradata && avctx->extradata_size > 0) {
    uint8_t *conv;
    int convLen, ret;
    if (csd_store(s, avctx->extradata, avctx->extradata_size) < 0)
      av_log(avctx, AV_LOG_WARNING, "byteVC2dec: csd parse failed\n");
    conv = av_malloc((size_t)avctx->extradata_size * 2 + 64);
    if (!conv)
      return AVERROR(ENOMEM);
    convLen = bytevc2_extradata_to_annexb(avctx->extradata,
                                          avctx->extradata_size,
                                          conv, avctx->extradata_size * 2 + 64);
    if (convLen <= 0) {
      av_log(avctx, AV_LOG_WARNING, "byteVC2dec: extradata_to_annexb failed\n");
      av_freep(&conv);
      return 0;
    }
    byteVC2dec_set_packet_pointer_info(s->pkt, "bs", conv);
    byteVC2dec_set_packet_integer_info(s->pkt, "bs_len", convLen);
    byteVC2dec_set_packet_integer_info(s->pkt, "opt", 0);
    byteVC2dec_set_packet_integer_info(s->pkt, "pts", 0);
    byteVC2dec_set_packet_integer_info(s->pkt, "flag", 0);
    byteVC2dec_set_packet_integer_info(s->pkt, "drop_frame", 0);
    byteVC2dec_set_packet_float_info(s->pkt, "drop_rate", 0.0f);
    ret = byteVC2dec_send_packet(s->ctx, s->pkt);
    if (ret != 0)
      av_log(avctx, AV_LOG_WARNING, "byteVC2dec: send csd failed, ret=%d\n", ret);
    av_freep(&conv);
  }
  return 0;
}

static av_cold int bytevc2_close(AVCodecContext *avctx)
{
  ByteVc2DecContext *s = avctx->priv_data;
  csd_clear(s);
  if (s->ctx)
    byteVC2dec_destroy(s->ctx);
  if (s->param)
    byteVC2dec_release_param(s->param);
  if (s->pkt)
    byteVC2dec_release_packet(s->pkt);
  if (s->frame)
    byteVC2dec_release_frame(s->frame);
  s->ctx = NULL; s->param = NULL; s->pkt = NULL; s->frame = NULL;
  av_freep(&s->convBuf);
  s->convCap = 0;
  return 0;
}

static av_cold void bytevc2_flush(AVCodecContext *avctx)
{
  ByteVc2DecContext *s = avctx->priv_data;
  if (!s->ctx)
    return;
  /* 实测 seek 契约: flush(ctx,1) 后必须 reconfig, 否则后续 decode 全部丢帧 */
  byteVC2dec_flush(s->ctx, 1);
  byteVC2dec_reconfig(s->ctx, s->param);
}

/* 单平面拷贝: 解码器行距 -> AVFrame 行距 */
static void copy_plane(uint8_t *dst, int dstStride, const uint8_t *src,
                       int srcStride, int width, int height)
{
  int r, bytes = FFMIN(srcStride, dstStride);
  for (r = 0; r < height; r++)
    memcpy(dst + (size_t)r * dstStride, src + (size_t)r * srcStride, bytes);
}

static int bytevc2_decode(AVCodecContext *avctx, AVFrame *frame,
                          int *got_frame, AVPacket *avpkt)
{
  ByteVc2DecContext *s = avctx->priv_data;
  int64_t ptsUs = 0;
  int ret, w, h, lsY, lsU, lsV;
  const uint8_t *pY, *pU, *pV;
  enum AVPixelFormat fmt;

  *got_frame = 0;
  if (!avpkt || !avpkt->data || avpkt->size <= 0)
    return 0;   /* draining: 同步直出无内部缓冲 */

  if (!ensure_conv(s, avpkt->size * 2 + 64))
    return AVERROR(ENOMEM);
  ret = convert_sample(s, avpkt->data, avpkt->size);
  if (ret <= 0) {
    av_log(avctx, AV_LOG_ERROR, "byteVC2dec: nal convert failed (len=%d)\n",
           avpkt->size);
    return AVERROR_INVALIDDATA;
  }

  /* pts 契约: 内部解码器需要显示序单调时间戳 (微秒), 用作发射序保证 */
  if (avpkt->pts != AV_NOPTS_VALUE) {
    if (avctx->pkt_timebase.num)
      ptsUs = av_rescale_q(avpkt->pts, avctx->pkt_timebase,
                           (AVRational) { 1, 1000000 });
    else
      ptsUs = avpkt->pts;
  }

  byteVC2dec_set_packet_pointer_info(s->pkt, "bs", s->convBuf);
  byteVC2dec_set_packet_integer_info(s->pkt, "bs_len", ret);
  byteVC2dec_set_packet_integer_info(s->pkt, "opt", 0);
  byteVC2dec_set_packet_integer_info(s->pkt, "pts", ptsUs);
  byteVC2dec_set_packet_integer_info(s->pkt, "flag", 0);
  byteVC2dec_set_packet_integer_info(s->pkt, "drop_frame", 0);
  byteVC2dec_set_packet_float_info(s->pkt, "drop_rate", 0.0f);

  ret = byteVC2dec_decode(s->ctx, s->pkt, s->frame);
  if (ret != 0) {
    av_log(avctx, AV_LOG_ERROR, "byteVC2dec: decode error ret=%#x\n", ret);
    return AVERROR_EXTERNAL;
  }
  /* 同步直出: got_frame 且 data 有效才是真帧 (无帧时 got_frame 可能残留) */
  if (!byteVC2dec_get_frame_integer_info(s->frame, "got_frame", 0) ||
      !(pY = byteVC2dec_get_frame_pointer_info(s->frame, "data", 0)))
    return avpkt->size;

  w   = (int)byteVC2dec_get_frame_integer_info(s->frame, "width", 0);
  h   = (int)byteVC2dec_get_frame_integer_info(s->frame, "height", 0);
  lsY = (int)byteVC2dec_get_frame_integer_info(s->frame, "linesize", 0);
  lsU = (int)byteVC2dec_get_frame_integer_info(s->frame, "linesize", 1);
  lsV = (int)byteVC2dec_get_frame_integer_info(s->frame, "linesize", 2);
  pU  = byteVC2dec_get_frame_pointer_info(s->frame, "data", 1);
  pV  = byteVC2dec_get_frame_pointer_info(s->frame, "data", 2);
  if (w <= 0 || h <= 0 || lsY <= 0 || !pU) {
    byteVC2dec_return_frame(s->ctx, s->frame);
    return AVERROR_EXTERNAL;
  }
  if (lsY < w) lsY = w;

  /* plane2 == NULL -> NV12 交错; 否则 I420 (YUV420P) */
  if (!pV) {
    fmt = AV_PIX_FMT_NV12;
    if (lsU <= 0) lsU = lsY / 2;
  } else {
    fmt = AV_PIX_FMT_YUV420P;
    if (lsU <= 0) lsU = lsY / 2;
    if (lsV <= 0) lsV = lsU;
  }

  frame->width  = w;
  frame->height = h;
  frame->format = fmt;
  /* 同步 avctx 尺寸 (get_buffer 校验 coded_width/coded_height)
   * 与像素格式 (ff_get_buffer 依据 avctx->pix_fmt 分配缓冲) */
  av_log(avctx, AV_LOG_DEBUG, "bvc2 frame %dx%d lsY=%d lsU=%d fmt=%d\n", w, h, lsY, lsU, fmt);
  ret = ff_set_dimensions(avctx, w, h);
  if (ret < 0) {
    av_log(avctx, AV_LOG_ERROR, "bvc2 ff_set_dimensions failed ret=%d\n", ret);
    byteVC2dec_return_frame(s->ctx, s->frame);
    return ret;
  }
  {
    enum AVPixelFormat candidates[2] = { fmt, AV_PIX_FMT_NONE };
    ret = ff_get_format(avctx, candidates);
    if (ret < 0) {
      byteVC2dec_return_frame(s->ctx, s->frame);
      return ret;
    }
    avctx->pix_fmt = ret;   /* ff_get_format 只返回选择, pix_fmt 需自行赋值 */
  }
  av_log(avctx, AV_LOG_DEBUG, "bvc2 pre-getbuf aw=%d ah=%d cw=%d ch=%d apf=%d mp=%lld fw=%d fh=%d\n",
         avctx->width, avctx->height, avctx->coded_width, avctx->coded_height,
         avctx->pix_fmt, (long long)avctx->max_pixels, frame->width, frame->height);
  ret = ff_get_buffer(avctx, frame, 0);
  if (ret < 0) {
    byteVC2dec_return_frame(s->ctx, s->frame);
    return ret;
  }

  if (fmt == AV_PIX_FMT_NV12) {
    /* Y 平面 + UV 交错平面 (dst 行距 >= src 行距时逐行拷贝) */
    copy_plane(frame->data[0], frame->linesize[0], pY, lsY, w, h);
    copy_plane(frame->data[1], frame->linesize[1], pU, lsU,
               FFMIN(lsU, frame->linesize[1]), (h + 1) / 2);
  } else {
    int uvH = (h + 1) / 2;
    copy_plane(frame->data[0], frame->linesize[0], pY, lsY, w, h);
    copy_plane(frame->data[1], frame->linesize[1], pU, lsU,
               FFMIN(lsU, frame->linesize[1]), uvH);
    copy_plane(frame->data[2], frame->linesize[2], pV, lsV,
               FFMIN(lsV, frame->linesize[2]), uvH);
  }
  /* 归还内部槽位 (环形缓冲仅 5 槽, 不归还即卡死) */
  byteVC2dec_return_frame(s->ctx, s->frame);

  /* 保留上游合成时间戳 (dts+ctts); 解码器按显示序直出, 序与 pts 一致 */
  frame->pts = avpkt->pts;
  *got_frame = 1;
  return avpkt->size;
}

const FFCodec ff_bytevc2_decoder = {
    .p.name          = "bytevc2",
    CODEC_LONG_NAME("ByteVC2 (software)"),
    .p.type          = AVMEDIA_TYPE_VIDEO,
    .p.id            = AV_CODEC_ID_BYTEVC2,
    .priv_data_size  = sizeof(ByteVc2DecContext),
    .init            = bytevc2_init,
    .close           = bytevc2_close,
    .flush           = bytevc2_flush,
    FF_CODEC_DECODE_CB(bytevc2_decode),
    .p.capabilities  = AV_CODEC_CAP_DR1,
    .caps_internal   = FF_CODEC_CAP_INIT_CLEANUP,
};
