/*
 * bytevc2_api.h
 *
 * 从 libttmplayer.so (vc2_dec.cpp) 对 libbyteVC2dec.so 的调用点逆向恢复的
 * 字节自研 ByteVC2 视频解码器 C API 契约。
 *
 * 结构体内部布局不透明, 一切字段访问均通过 string-key 的 info 接口完成:
 *   byteVC2dec_set_param_integer_info(param, key, value)
 *   byteVC2dec_set_packet_integer_info(pkt, key, value)
 *   byteVC2dec_set_packet_pointer_info(pkt, key, ptr)
 *   byteVC2dec_set_packet_float_info(pkt, key, value)
 *   byteVC2dec_get_frame_integer_info(frame, key, idx)
 *   byteVC2dec_get_frame_pointer_info(frame, key, idx)
 */
#ifndef BYTEVC2_API_H
#define BYTEVC2_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct byteVC2dec_param   byteVC2dec_param;   /* 0x70 字节, 不透明 */
typedef struct byteVC2dec_ctx     byteVC2dec_ctx;     /* 0x2808 字节, 不透明 */
typedef struct byteVC2dec_packet  byteVC2dec_packet;  /* 0x58 字节, 不透明 */
typedef struct byteVC2dec_frame   byteVC2dec_frame;   /* 0x98 字节, 不透明 */

/* ---- 日志 ---- */
typedef void (*byteVC2_log_cb)(void *userdata, int level, const char *tag, const char *msg);
void byteVC2_set_log_callback(byteVC2_log_cb cb, void *userdata);

/* ---- 生命周期 ---- */
/* 版本号写入调用方缓冲区 (至少 8 字节), 实测写入 "0.9.7.1\0" */
void byteVC2dec_get_version(char *out /* [>=8] */);

byteVC2dec_param *byteVC2dec_get_default_param(void);   /* malloc(0x70) */
void              byteVC2dec_release_param(byteVC2dec_param *param);
void              byteVC2dec_print_param(const byteVC2dec_param *param);

int  byteVC2dec_set_param_integer_info(byteVC2dec_param *param, const char *key, int value);
int  byteVC2dec_set_param_float_info(byteVC2dec_param *param, const char *key, float value);
int  byteVC2dec_set_param_pointer_info(byteVC2dec_param *param, const char *key, void *value);

/* 成功返回非 NULL 上下文, 失败返回 NULL */
byteVC2dec_ctx *byteVC2dec_create(const byteVC2dec_param *param);
void            byteVC2dec_destroy(byteVC2dec_ctx *ctx);

/* 重配置 (分辨率/参数变化时), 参数同 create */
int byteVC2dec_reconfig(byteVC2dec_ctx *ctx, const byteVC2dec_param *param);

/* ---- packet ---- */
byteVC2dec_packet *byteVC2dec_alloc_packet(void);        /* malloc(0x58) */
void               byteVC2dec_release_packet(byteVC2dec_packet *pkt);

int byteVC2dec_set_packet_integer_info(byteVC2dec_packet *pkt, const char *key, int64_t value);
int byteVC2dec_set_packet_float_info(byteVC2dec_packet *pkt, const char *key, float value);
int byteVC2dec_set_packet_pointer_info(byteVC2dec_packet *pkt, const char *key, void *value);

/* ---- frame ---- */
byteVC2dec_frame *byteVC2dec_alloc_frame(void);          /* malloc(0x98) */
void              byteVC2dec_release_frame(byteVC2dec_frame *frame);

int64_t byteVC2dec_get_frame_integer_info(const byteVC2dec_frame *frame, const char *key, int idx);
void   *byteVC2dec_get_frame_pointer_info(const byteVC2dec_frame *frame, const char *key, int idx);
float   byteVC2dec_get_frame_float_info(const byteVC2dec_frame *frame, const char *key);

/* ---- 解码 ----
 * 同步模式: send_packet -> (可选) get_frame
 * 直通模式: decode(ctx, pkt, frame) 一次完成
 * 返回 0 表示调用成功, 非 0 为错误码 (见 ttmplayer "dec err(%x) %d")
 */
int byteVC2dec_send_packet(byteVC2dec_ctx *ctx, byteVC2dec_packet *pkt);
int byteVC2dec_decode(byteVC2dec_ctx *ctx, byteVC2dec_packet *pkt, byteVC2dec_frame *frame);

/* 取一帧已解码数据到 frame; 无帧时 frame->got_frame=0, 仍返回 0 */
int byteVC2dec_get_frame(byteVC2dec_ctx *ctx, byteVC2dec_frame *frame);

/* 归还 frame 内部槽位 (内部环形缓冲共 5 槽, 不归还会导致解码器卡死) */
int byteVC2dec_return_frame(byteVC2dec_ctx *ctx, byteVC2dec_frame *frame);

/* 超分帧 (sr_mode), 普通播放可忽略 */
int byteVC2dec_get_sr_frame(byteVC2dec_ctx *ctx, byteVC2dec_frame *frame);

/* ---- flush ----
 * flush(ctx, flags): 丢弃所有缓冲帧; flags bit0=1 时同时清理缓存并
 *   重配置解码器内部状态 (ttmplayer seek/复用路径恒传 1, 反汇编
 *   0xd095c / 0x202fe0 调用点确认); 传 0 或未定义值会导致后续 decode
 *   持续返回 1 (实测)。
 * flush_keep_buffer(ctx): 保留已缓冲帧 (用于码流切换)。
 */
int byteVC2dec_flush(byteVC2dec_ctx *ctx, int flags);
int byteVC2dec_flush_keep_buffer(byteVC2dec_ctx *ctx);

/* ---- 码流辅助 ----
 * 输入为 Annex-B (00 00 00 01 起始码) 连续码流时, 按访问单元切分
 */
int byteVC2_get_next_access_unit(const uint8_t *buf, int len);

/* ---- 诊断 ---- */
void byteVC2dec_dump_video_info(byteVC2dec_ctx *ctx);
void byteVC2dec_dump_decoding_status(byteVC2dec_ctx *ctx);

/* ---- 已恢复的 string-key 枚举 (来源: libttmplayer.so 交叉引用) ----
 *
 * [param 整型 key]  set_param_integer_info:
 *   "threads"              解码线程数 (ttmplayer: CPU 核数, 默认 8)
 *   "max_threads"          最大线程数 (ttmplayer 默认 8)
 *   "low_latency"          低延迟模式 (直播 1, 点播 0)
 *   "wpp_mode"             Wavefront 并行 (1 开启)
 *   "parallel_frames"      帧级并行度 (>=1 才设置)
 *   "max_parallel_frames"  帧级并行上限
 *   "advanced_simd"        SIMD 优化 (>=1 才设置)
 *   "output_method"        输出方式
 *   "hardware_buffer"      AHardwareBuffer 直出 (0 = CPU YUV)
 *   "hwb_attach_color_info" hardbuffer 携带色彩信息
 *   "max_cached_frames"    内部缓存帧上限
 *   "gpu_mode"             GPU 协处理
 *   "output_time"          输出时序
 *   "thread_reduce"        线程收敛
 *   "fpp_less_sync"        省同步
 *   "thread_priority"      解码线程优先级
 *
 * [packet 整型 key]  set_packet_integer_info:
 *   "bs_len"       码流长度 (必需)
 *   "opt"          优化标记, 恒传 0
 *   "pts"          显示时间戳 (微秒)
 *   "flag"         0=普通帧; 参数集(稀疏)包传 0, pts 同样为 0
 *   "drop_frame"   丢帧标记, 恒传 0
 * [packet 浮点 key]
 *   "drop_rate"    主动丢帧率 0.0~1.0, 恒传 0.0f
 * [packet 指针 key]
 *   "bs"           码流指针 (Annex-B, 00 00 00 01 起始码)
 *
 * [frame 整型 key]  get_frame_integer_info:
 *   "width"        (idx=0) 帧宽
 *   "height"       (idx=0) 帧高
 *   "linesize"     (idx=0/1/2) 各平面 stride
 *   "stride"       (idx=0/1) 同 linesize 别名
 *   "got_frame"    (idx=0) 本次是否得到帧
 *   "freeze"       冻结/重复帧标记
 *   "sr_mode"      超分模式标记
 *   "color_info_present_flag" 色彩信息存在
 *   "video_full_range_flag"   full range
 * [frame 指针 key]  get_frame_pointer_info:
 *   "data"         (idx=0/1/2) Y/U/V 平面指针;
 *                  idx==2 为 NULL 时平面 1 为 NV12 交错 UV
 *   "hardware_buffer" (idx=0) AHardwareBuffer*, 仅 hardware_buffer=1 时有效
 */
#ifdef __cplusplus
}
#endif
#endif /* BYTEVC2_API_H */
