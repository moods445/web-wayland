/*
 * wwcap 音频采集（PipeWire monitor → Opus → Unix DGRAM）
 *
 * 只做下行：抓默认 sink 的 monitor（整机混音），20ms 一帧 Opus，
 * 每包额外带上一帧做 RED 冗余，单包丢失可由下一包完整恢复。
 * 上行（浏览器麦克风 → ww-mic）是二期的 ww_audio_* 扩展点，见 audio.c 末尾。
 */
#ifndef WW_AUDIO_H
#define WW_AUDIO_H

#include <stdbool.h>
#include <stdint.h>

struct ww_audio;

/* fd 由调用方创建（非阻塞 Unix DGRAM），sock_path 只用于建立目的地址。
   返回 NULL 表示初始化失败（缺 PipeWire 服务/Opus 编码器），调用方应降级为无声串流。 */
struct ww_audio *ww_audio_create(int fd, const char *sock_path, int bitrate);
void ww_audio_destroy(struct ww_audio *a);

/* 客户端声明需要下行音频时才启采集，避免无人观看时空跑编码器 */
void ww_audio_set_enabled(struct ww_audio *a, bool on);

/* 冗余份数：0 = 关闭，>=1 = 每包携带 seq-1（抗单包丢失） */
void ww_audio_set_redundancy(struct ww_audio *a, int n);

void ww_audio_stats(struct ww_audio *a, uint64_t *pkts, uint64_t *bytes, uint64_t *dropped);

#endif /* WW_AUDIO_H */
