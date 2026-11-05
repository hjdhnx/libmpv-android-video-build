/*
 * bytevc2_nal.c
 *
 * ByteVC2 码流格式转换。逻辑从 libttmplayer.so 中的
 * convertByteVC2Extradata (vc2_dec.cpp @0x202140) 与
 * convert_bytevc2_nal_units (@0x202400) 反汇编恢复:
 *
 *  - 解码器输入 ("bs") 必须是 Annex-B: 00 00 00 01 + NAL 连续排列;
 *  - MP4/FLV 封装的 sample 为长度前缀式 NAL, 需逐个替换为起始码;
 *  - extradata 为 mpvC/avcC 风格参数集容器:
 *      byte0   : 配置标记
 *      byte1   : bit0 = 存在扩展头, bits1..3 = NAL 长度域字节数-1
 *      (bit0=0) byte2 = 参数组数 groupCount
 *      每组:    nalType(1B) + nalCount(2B, 大端) + nalCount 个
 *               { nalLen(2B, 大端) + NAL }
 *    转换即把每个 NAL 前加 00 00 00 01 (反汇编中写 0,0,0,1 后 memcpy)。
 */
#include "bytevc2_nal.h"
#include <string.h>
#include <stdint.h>

#define SC3(p) ((p)[0] == 0 && (p)[1] == 0 && (p)[2] == 1)
#define SC4(p) ((p)[0] == 0 && (p)[1] == 0 && (p)[2] == 0 && (p)[3] == 1)

int bytevc2_is_annexb(const uint8_t *data, int len) {
  if (len >= 4 && SC4(data)) return 1;
  if (len >= 3 && SC3(data)) return 1;
  return 0;
}

/* 长度前缀式 -> Annex-B。lengthSize 为 1..4, 大端 (mp4 封装惯例)。
 * 输出追加写入 *out, 返回写入字节数; 空间不足返回 -1。 */
int bytevc2_length_prefixed_to_annexb(const uint8_t *data, int len, int lengthSize,
                                      uint8_t *out, int outCap) {
  static const uint8_t sc[4] = {0, 0, 0, 1};
  int pos = 0, written = 0;
  if (lengthSize < 1 || lengthSize > 4) return -1;
  if (bytevc2_is_annexb(data, len)) {
    /* 已是 Annex-B, 原样拷贝 */
    if (len > outCap) return -1;
    memcpy(out, data, len);
    return len;
  }
  while (pos + lengthSize <= len) {
    int32_t nalLen = 0;
    int i;
    for (i = 0; i < lengthSize; i++) nalLen = (nalLen << 8) | data[pos + i];
    pos += lengthSize;
    if (nalLen <= 0 || pos + nalLen > len) return -1;
    if (written + 4 + nalLen > outCap) return -1;
    memcpy(out + written, sc, 4);
    memcpy(out + written + 4, data + pos, nalLen);
    written += 4 + nalLen;
    pos += nalLen;
  }
  if (pos != len) return -1; /* 尾部残缺 */
  return written;
}

/* bv2C 风格 extradata -> Annex-B (经 bvc2_plain.mp4 实测校准):
 *
 *   [0]       配置版本 = 1
 *   [1]       bit0 = 存在扩展头; (byte1>>1)&3 + 1 = NAL 长度域字节数 (0x07 -> 4)
 *   [2..10]   编码参数/保留
 *   [12..13]  宽 (2B 大端), [14..15] 高 (2B 大端), [16..18] 附加信息
 *   [19]      起第一个 section: [type 1B][count 2B 大端]
 *             + count × { nalLen(2B 大端) + NAL }
 *   实测类型: 0x8f=序列参数(1个), 0x90=特征描述(7个), 0x17=版本信息(1个)
 *   转换 = 所有 section 的全部 NAL 各前置 00 00 00 01 拼接。
 *
 * 若 extradata 本身是 Annex-B 则原样拷贝。 */
int bytevc2_extradata_to_annexb(const uint8_t *data, int len,
                                uint8_t *out, int outCap) {
  static const uint8_t sc[4] = {0, 0, 0, 1};
  int written = 0, groups, g, pos;
  uint8_t flags;

  if (bytevc2_is_annexb(data, len)) {
    if (len > outCap) return -1;
    memcpy(out, data, len);
    return len;
  }
  if (len < 22) return -1; /* 反汇编: 输入至少 0x16 字节 */

  flags = data[1];

  /* ---- bv2C v1 主路径: 19B 扩展头 + [type][count][len+NAL] sections ---- */
  if (data[0] == 1 && len >= 24) {
    int ok = 1, sections = 0;
    pos = 19;
    while (ok && pos + 3 <= len && sections < 8) {
      int count = (data[pos + 1] << 8) | data[pos + 2];
      pos += 3;
      if (count <= 0 || count > 64) { ok = 0; break; }
      while (count-- > 0) {
        int nalLen;
        if (pos + 2 > len) { ok = 0; break; }
        nalLen = (data[pos] << 8) | data[pos + 1];
        pos += 2;
        if (nalLen <= 0 || pos + nalLen > len) { ok = 0; break; }
        if (written + 4 + nalLen > outCap) { ok = 0; break; }
        memcpy(out + written, sc, 4);
        memcpy(out + written + 4, data + pos, nalLen);
        written += 4 + nalLen;
        pos += nalLen;
      }
      if (!ok) break;
      sections++;
    }
    if (ok && written > 0 && pos == len) return written;
    written = 0; /* 校验失败, 回落 legacy 解析 */
  }

  /* bit0 = 1: 存在扩展头, 按 LEB128/OBU 长度域跳过后再进入参数组表。
   * 主播放场景极少出现; 这里做保守处理: 直接扫描起始码。 */
  if (flags & 1) {
    int p = 2;
    while (p < len) {
      if (SC4(data + p)) { /* 从第一个起始码处整体透传 */
        int n = len - p;
        if (n > outCap) return -1;
        memcpy(out, data + p, n);
        return n;
      }
      p++;
    }
    return -1;
  }

  groups = data[2];
  pos = 3;
  for (g = 0; g < groups && pos + 3 <= len; g++) {
    int count = (data[pos + 1] << 8) | data[pos + 2]; /* 2B 大端 */
    pos += 3; /* nalType(1B) + count(2B) */
    while (count-- > 0) {
      int nalLen;
      if (pos + 2 > len) return -1;
      nalLen = (data[pos] << 8) | data[pos + 1];
      pos += 2;
      if (nalLen < 0 || pos + nalLen > len) return -1;
      if (written + 4 + nalLen > outCap) return -1;
      memcpy(out + written, sc, 4);
      memcpy(out + written + 4, data + pos, nalLen);
      written += 4 + nalLen;
      pos += nalLen;
    }
  }
  return written;
}

/* Annex-B 码流按访问单元切分辅助: 返回从 start 开始的下一个
 * AU 边界 (首帧类型变化处), 复刻 byteVC2_get_next_access_unit 的语义。
 * 简化实现: 以 VCL NAL 类型变化 / 新的参数集作为 AU 边界。
 * 若解不了返回 -1 (整段视为一个 AU)。 */
int bytevc2_next_access_unit(const uint8_t *data, int len, int start) {
  int pos = start, firstVcl = -1;
  if (!bytevc2_is_annexb(data, len)) return -1;
  while (pos + 4 <= len) {
    if (SC4(data + pos)) {
      int nalPos = pos + 4;
      if (nalPos >= len) break;
      /* 跳过起始码前的零填充 */
      if (firstVcl >= 0 && !SC3(data + nalPos)) {
        /* 遇到下一个 NAL: 若当前 NAL 是 VCL 且新 NAL 类型不同则切分 */
        return nalPos;
      }
    }
    pos++;
  }
  return -1;
}
