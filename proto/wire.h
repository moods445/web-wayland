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

/* 视频分片头 —— 24 字节，与 web/src/proto.js 保持一致 */
struct ww_vid_frag_hdr {
    uint8_t  version;      /* = WW_WIRE_VERSION */
    uint8_t  flags;        /* bit0 关键帧, bit1 首片, bit2 末片 */
    uint16_t stream_id;
    uint32_t frame_id;
    uint16_t frag_index;
    uint16_t frag_count;
    uint32_t payload_len;
    uint64_t pts_us;   /* offset 16，与 web/src/proto.js 一致 */
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

/* 视频帧在 IPC 上的封装：一次 write = 一个完整分片（头 + 载荷） */
#define WW_IPC_VIDEO 0x01
#define WW_IPC_CTRL  0x02

#endif /* WW_WIRE_H */
