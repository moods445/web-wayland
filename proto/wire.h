/*
 * web-wayland 串流协议（视频走不可靠 datagram，控制走可靠流）
 *
 * 视频：每个 QUIC datagram = ww_vid_frag_hdr + payload(<=1100B)
 * 控制：每条消息 = uint32 LE 长度 + UTF-8 JSON
 *
 * 设计要点：
 *  - 单包不超过 1100 字节，避免 QUIC/IP 分片
 *  - pts_us 是抓帧时刻（CLOCK_MONOTONIC 换算到 unix 微秒），前端据此丢弃过期帧并算端到端延迟
 *  - 关键帧标记让前端在丢片后能快速跳到下一个可用帧
 */

#ifndef WW_WIRE_H
#define WW_WIRE_H

#include <stdint.h>

#define WW_WIRE_VERSION 1
#define WW_MAX_PAYLOAD 1100
#define WW_VID_HDR_SIZE 24

/* 视频分片头 —— 24 字节（历史自建链路协议，已无使用者）*/
struct ww_vid_frag_hdr {
    uint8_t  version;      /* = WW_WIRE_VERSION */
    uint8_t  flags;        /* bit0 关键帧, bit1 首片, bit2 末片 */
    uint16_t stream_id;
    uint32_t frame_id;
    uint16_t frag_index;
    uint16_t frag_count;
    uint32_t payload_len;
    uint64_t pts_us;   /* offset 16 */
} __attribute__((packed));

#define WW_FRAG_KEY       0x01
#define WW_FRAG_FIRST     0x02
#define WW_FRAG_LAST      0x04

/* 控制消息类型（JSON 里的 "t" 字段） */
#define WW_MSG_HELLO        "hello"        /* client→server: {t,w,h,dpr,fps} */
#define WW_MSG_CONFIGURE    "configure"    /* server→client: {t,w,h,fps,qp,codec} */
#define WW_MSG_READY        "ready"        /* client→server: 解码器已重建 */
#define WW_MSG_KEYFRAME     "keyframe"     /* client→server: 请求 IDR */
#define WW_MSG_STATS        "stats"        /* client→server: {loss,rtt,decode_ms,queue} */
#define WW_MSG_POINTER      "pointer"      /* client→server: {mode,dx,dy,x,y,w,h,btn} */
#define WW_MSG_KEY          "key"          /* client→server: {code,down} */
#define WW_MSG_MOD          "mod"          /* client→server: {shift,ctrl,alt,super} */
#define WW_MSG_STRATEGY     "strategy"     /* client→server: {v} 0=平衡 1=流畅 2=画质 3=自定义 */
#define WW_MSG_CHROMA       "chroma"       /* client→server: {v} 1=4:4:4 0=4:2:0 */

/* 视频帧在 IPC 上的封装：一次 write = 一个完整分片（头 + 载荷） */
#define WW_IPC_VIDEO 0x01
#define WW_IPC_CTRL  0x02
#define WW_IPC_AUDIO 0x03       /* wwcap → gw：音频包（头+主帧+冗余帧） */
#define WW_IPC_AUDIO_UP 0x04    /* gw → wwcap：上行麦克风音频包（二期） */

/* ── 音频 ────────────────────────────────────────────────────────
 * datagram 首字节同时充当媒体类型判别：视频 = 1（ww_vid_frag_hdr），
 * 音频 = 2（ww_aud_hdr）。这样音视频共用一个 datagram 通道，
 * 前端 #onDatagram 只看第一个字节即可分流，不用再加一层隧道头。
 *
 * 时间轴刻意不跨机对钟：22.7ms 一包使 seq 本身就是一个稳定时钟，
 * 客户端用 seq 推算播放位置，服务端 ts_ms 只用于本机排障与延迟估计。
 */
#define WW_AUDIO_VERSION 2
#define WW_AUDIO_HDR_SIZE 16

#define WW_AUDIO_RATE 48000
#define WW_AUDIO_CH 2
#define WW_AUDIO_FRAME_MS 20
#define WW_AUDIO_FRAME_SAMPLES 960          /* 48000 * 20 / 1000 */
#define WW_AUDIO_BITRATE 64000              /* 立体声总码率 */

/* 音频包 —— 16 字节（历史自建链路协议，已无使用者）*/
struct ww_aud_hdr {
    uint8_t  version;    /* = WW_AUDIO_VERSION */
    uint8_t  flags;      /* bit0 主帧有效, bit1 冗余帧有效 */
    uint16_t seq;        /* 20ms 帧序号，回绕后客户端按有符号差比较 */
    uint16_t len;        /* 主帧（Opus）字节数 */
    uint16_t red_len;    /* 冗余帧（seq-1）字节数，0 = 无 */
    uint32_t ts_ms;      /* 采集时刻（CLOCK_MONOTONIC 毫秒），仅排障 */
    uint32_t reserved;   /* 预留：声道路由/上行回传 */
} __attribute__((packed));

#define WW_AUD_MAIN 0x01
#define WW_AUD_RED  0x02

#endif /* WW_WIRE_H */
