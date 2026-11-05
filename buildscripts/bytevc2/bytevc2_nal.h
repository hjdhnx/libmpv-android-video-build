/*
 * bytevc2_nal.h
 */
#ifndef BYTEVC2_NAL_H
#define BYTEVC2_NAL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int bytevc2_is_annexb(const uint8_t *data, int len);

/* 长度前缀 NAL -> Annex-B(00 00 00 01), 返回输出长度, 失败 -1 */
int bytevc2_length_prefixed_to_annexb(const uint8_t *data, int len, int lengthSize,
                                      uint8_t *out, int outCap);

/* mpvC/avcC 风格 extradata 参数集容器 -> Annex-B, 失败 -1 */
int bytevc2_extradata_to_annexb(const uint8_t *data, int len,
                                uint8_t *out, int outCap);

int bytevc2_next_access_unit(const uint8_t *data, int len, int start);

#ifdef __cplusplus
}
#endif
#endif
