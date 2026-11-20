/*
 * wwcap 音频采集：PipeWire monitor → S16 交织 → Opus → RED 打包 → Unix DGRAM
 *
 * 设计要点（与 docs/TEST-CASES.md K 节、README 音频章节对应）：
 *  - 抓虚拟 sink ww-desk 的 monitor（stream.capture.sink=true），只含应用声音；
 *    不能直接抓硬件 sink：部分声卡默认开启 Loopback Mixing，会把麦克风/环境声
 *    混进 monitor，客户端听到的就是服务器麦克风（见 TEST-CASES K-08）
 *  - 20ms / 960 samples 一帧，包内追加 seq-1 的完整 Opus 帧（RED），
 *    单包丢失可由下一包无损恢复；多包丢失交给前端淡出遮挡
 *  - 低延迟优先：node.latency=480/48000（10ms），编码在 RT 回调内完成，
 *    避免多一层队列把抖动放大
 *  - 编码与发送都在 PipeWire 线程里做，主循环不需要额外 fd
 */
#define _POSIX_C_SOURCE 200809L

#include "audio.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <opus.h>
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>

#include "../../proto/wire.h"

#define ACC_MAX_FRAMES (WW_AUDIO_FRAME_SAMPLES * 8)
#define PKT_MAX 1275   /* RFC 6716：单个 Opus 包上限 */
/* 虚拟输出 sink，由 scripts/pipewire/50-ww-desk.conf 创建；应用默认播到这里 */
#define WW_AUDIO_TARGET_NODE "ww-desk"

struct ww_audio {
    struct pw_thread_loop *loop;
    struct pw_stream *stream;
    struct spa_hook listener;

    int fd;
    struct sockaddr_un addr;

    OpusEncoder *enc;

    /* 已收到的 PCM 累积（S16 交织），凑满 960 帧就编码一包 */
    int16_t acc[ACC_MAX_FRAMES * WW_AUDIO_CH];
    int acc_frames;

    /* 上一帧的编码结果，用于下一包的 RED 冗余 */
    uint8_t prev[PKT_MAX];
    int prev_len;

    uint16_t seq;
    int redundancy;

    atomic_int enabled;
    atomic_uint_fast64_t pkts, bytes, dropped;
};

static uint32_t monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)((uint64_t)ts.tv_sec * 1000ull + (uint64_t)ts.tv_nsec / 1000000ull);
}

static void encode_and_send(struct ww_audio *a)
{
    if (!atomic_load_explicit(&a->enabled, memory_order_relaxed)) {
        a->acc_frames = 0;
        return;
    }

    uint8_t main_pkt[PKT_MAX];
    int n = opus_encode(a->enc, a->acc, WW_AUDIO_FRAME_SAMPLES, main_pkt, sizeof(main_pkt));
    a->acc_frames = 0;
    if (n <= 0) {
        fprintf(stderr, "[aud] opus_encode 失败: %s\n", opus_strerror(n));
        return;
    }

    uint8_t out[WW_MAX_PAYLOAD];
    struct ww_aud_hdr h;
    memset(&h, 0, sizeof(h));
    h.version = WW_AUDIO_VERSION;
    h.seq = a->seq;
    h.ts_ms = monotonic_ms();
    h.len = (uint16_t)n;
    h.flags = WW_AUD_MAIN;

    size_t total = (size_t)n;
    memcpy(out + WW_AUDIO_HDR_SIZE, main_pkt, (size_t)n);

    if (a->redundancy > 0 && a->prev_len > 0
        && WW_AUDIO_HDR_SIZE + total + (size_t)a->prev_len <= WW_MAX_PAYLOAD) {
        memcpy(out + WW_AUDIO_HDR_SIZE + total, a->prev, (size_t)a->prev_len);
        h.red_len = (uint16_t)a->prev_len;
        h.flags |= WW_AUD_RED;
        total += (size_t)a->prev_len;
    }

    memcpy(out, &h, sizeof(h));

    /* 冗余帧是「上一帧」：必须在打包之后再滚动 prev */
    memcpy(a->prev, main_pkt, (size_t)n);
    a->prev_len = n;
    a->seq++;

    ssize_t r = sendto(a->fd, out, WW_AUDIO_HDR_SIZE + total, MSG_DONTWAIT,
                       (struct sockaddr *)&a->addr, sizeof(a->addr));
    if (r < 0) {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ECONNREFUSED && errno != ENOENT) {
            atomic_fetch_add(&a->dropped, 1);
        } else {
            atomic_fetch_add(&a->dropped, 1);   /* 网关没起/缓冲满：丢包但继续采 */
        }
        return;
    }
    atomic_fetch_add(&a->pkts, 1);
    atomic_fetch_add(&a->bytes, (uint64_t)r);
}

static void feed(struct ww_audio *a, const int16_t *pcm, int frames)
{
    while (frames > 0) {
        int room = WW_AUDIO_FRAME_SAMPLES - a->acc_frames;
        if (room <= 0) {
            encode_and_send(a);
            continue;
        }
        int n = frames < room ? frames : room;
        memcpy(a->acc + a->acc_frames * WW_AUDIO_CH, pcm,
               (size_t)n * WW_AUDIO_CH * sizeof(int16_t));
        a->acc_frames += n;
        pcm += (size_t)n * WW_AUDIO_CH;
        frames -= n;
        if (a->acc_frames >= WW_AUDIO_FRAME_SAMPLES) {
            encode_and_send(a);
        }
    }
}

static void on_process(void *data)
{
    struct ww_audio *a = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(a->stream);
    if (!b) {
        return;
    }
    struct spa_buffer *buf = b->buffer;
    struct spa_data *d = &buf->datas[0];
    if (d->data && d->chunk && d->chunk->size) {
        uint32_t off = d->chunk->offset;
        uint32_t size = d->chunk->size;
        /* 只认我们协商到的 S16 交织格式；别的格式在连上时已经打过日志 */
        const int16_t *pcm = (const int16_t *)((const uint8_t *)d->data + off);
        int frames = (int)(size / (sizeof(int16_t) * WW_AUDIO_CH));
        if (frames > 0) {
            feed(a, pcm, frames);
        }
    }
    pw_stream_queue_buffer(a->stream, b);
}

static void on_state_changed(void *data, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error)
{
    struct ww_audio *a = data;
    (void)a;
    (void)old;
    if (state == PW_STREAM_STATE_ERROR) {
        fprintf(stderr, "[aud] PipeWire 流错误: %s\n", error ? error : "unknown");
    } else if (state == PW_STREAM_STATE_STREAMING) {
        fprintf(stderr, "[aud] PipeWire monitor 已连上\n");
    }
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .process = on_process,
};

struct ww_audio *ww_audio_create(int fd, const char *sock_path, int bitrate)
{
    if (fd < 0 || !sock_path) {
        return NULL;
    }

    struct ww_audio *a = calloc(1, sizeof(*a));
    if (!a) {
        return NULL;
    }
    a->fd = fd;
    a->addr.sun_family = AF_UNIX;
    snprintf(a->addr.sun_path, sizeof(a->addr.sun_path), "%s", sock_path);
    a->redundancy = 1;
    a->seq = (uint16_t)(monotonic_ms() & 0xffff);
    /* 默认不发：等网关/客户端发 {"t":"astream","rx":1} 才真正编码发送，
       这样没人在看的时候不会白烧 CPU（见 TEST-CASES K-02） */
    atomic_store(&a->enabled, 0);

    int err = OPUS_OK;
    a->enc = opus_encoder_create(WW_AUDIO_RATE, WW_AUDIO_CH, OPUS_APPLICATION_AUDIO, &err);
    if (!a->enc || err != OPUS_OK) {
        fprintf(stderr, "[aud] opus_encoder_create 失败: %s\n", opus_strerror(err));
        free(a);
        return NULL;
    }
    int br = bitrate > 0 ? bitrate : WW_AUDIO_BITRATE;
    opus_encoder_ctl(a->enc, OPUS_SET_BITRATE(br));
    /* 延迟最低优先：复杂度压到 5（音质略降，CPU 减半），20ms 帧是 FEC 效率最好的档位 */
    opus_encoder_ctl(a->enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(a->enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(a->enc, OPUS_SET_PACKET_LOSS_PERC(10));
    opus_encoder_ctl(a->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    opus_encoder_ctl(a->enc, OPUS_SET_DTX(0));

    pw_init(NULL, NULL);
    a->loop = pw_thread_loop_new("wwcap-audio", NULL);
    if (!a->loop) {
        fprintf(stderr, "[aud] pw_thread_loop_new 失败\n");
        opus_encoder_destroy(a->enc);
        free(a);
        return NULL;
    }

    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Music",
        PW_KEY_NODE_NAME, "wwcap-audio",
        PW_KEY_NODE_DESCRIPTION, "web-wayland 桌面音频",
        PW_KEY_STREAM_CAPTURE_SINK, "true",     /* 抓 sink monitor = 应用混音 */
        PW_KEY_TARGET_OBJECT, WW_AUDIO_TARGET_NODE, /* 只要 ww-desk，避开硬件/麦克风 */
        PW_KEY_NODE_LATENCY, "480/48000",       /* 10ms quantum，低延迟优先 */
        NULL);

    pw_thread_loop_lock(a->loop);
    a->stream = pw_stream_new_simple(pw_thread_loop_get_loop(a->loop), "wwcap-audio",
                                     props, &stream_events, a);
    if (!a->stream) {
        pw_thread_loop_unlock(a->loop);
        fprintf(stderr, "[aud] pw_stream_new_simple 失败（PipeWire 未运行？）\n");
        pw_thread_loop_destroy(a->loop);
        opus_encoder_destroy(a->enc);
        free(a);
        return NULL;
    }

    uint8_t pbuf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(pbuf, sizeof(pbuf));
    struct spa_audio_info_raw info;
    memset(&info, 0, sizeof(info));
    info.format = SPA_AUDIO_FORMAT_S16;
    info.rate = WW_AUDIO_RATE;
    info.channels = WW_AUDIO_CH;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;

    const struct spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    int r = pw_stream_connect(a->stream, PW_DIRECTION_INPUT, PW_ID_ANY,
                              PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                  PW_STREAM_FLAG_RT_PROCESS,
                              params, 1);
    pw_thread_loop_unlock(a->loop);
    if (r < 0) {
        fprintf(stderr, "[aud] pw_stream_connect 失败: %d\n", r);
        ww_audio_destroy(a);
        return NULL;
    }

    if (pw_thread_loop_start(a->loop) != 0) {
        fprintf(stderr, "[aud] pw_thread_loop_start 失败\n");
        ww_audio_destroy(a);
        return NULL;
    }

    fprintf(stderr, "[aud] 采集已启动：48kHz 立体声 Opus %d bps，20ms 帧，RED 冗余 %d\n",
            br, a->redundancy);
    return a;
}

void ww_audio_destroy(struct ww_audio *a)
{
    if (!a) {
        return;
    }
    if (a->loop) {
        pw_thread_loop_stop(a->loop);   /* 先停 RT 线程，再释放回调会碰到的对象 */
        pw_thread_loop_lock(a->loop);
        if (a->stream) {
            pw_stream_destroy(a->stream);
            a->stream = NULL;
        }
        pw_thread_loop_unlock(a->loop);
        pw_thread_loop_destroy(a->loop);
        a->loop = NULL;
    }
    if (a->enc) {
        opus_encoder_destroy(a->enc);
        a->enc = NULL;
    }
    pw_deinit();
    free(a);
}

void ww_audio_set_enabled(struct ww_audio *a, bool on)
{
    if (!a) {
        return;
    }
    atomic_store(&a->enabled, on ? 1 : 0);
    if (!on) {
        a->acc_frames = 0;   /* 重开时从新的一帧开始，不补发陈旧音频 */
    }
    if (a->stream && a->loop) {
        pw_thread_loop_lock(a->loop);
        pw_stream_set_active(a->stream, on);
        pw_thread_loop_unlock(a->loop);
    }
}

void ww_audio_set_redundancy(struct ww_audio *a, int n)
{
    if (!a) {
        return;
    }
    if (n < 0) n = 0;
    if (n > 1) n = 1;   /* 单包只带一帧冗余；>1 视为开启 */
    a->redundancy = n;
}

void ww_audio_stats(struct ww_audio *a, uint64_t *pkts, uint64_t *bytes, uint64_t *dropped)
{
    if (!a) {
        if (pkts) *pkts = 0;
        if (bytes) *bytes = 0;
        if (dropped) *dropped = 0;
        return;
    }
    if (pkts) *pkts = atomic_load(&a->pkts);
    if (bytes) *bytes = atomic_load(&a->bytes);
    if (dropped) *dropped = atomic_load(&a->dropped);
}
