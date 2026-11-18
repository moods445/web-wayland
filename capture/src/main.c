/*
 * wwcap —— 抓帧 + 编码 + 输入注入
 *
 * 数据通路：
 *   Hyprland --ext-image-copy-capture(dmabuf)--> 本进程 --VPP+VAAPI--> Annex-B
 *        --Unix DGRAM 分片--> wwgw --WebTransport--> 浏览器
 *   浏览器 --WebTransport 控制流--> wwgw --Unix DGRAM JSON--> 本进程 --virtual-kb/ptr--> Hyprland
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/sysmacros.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <gbm.h>
#include <xf86drm.h>
#include <drm_fourcc.h>

#include <wayland-client.h>

#include "capture_shm.h"
#include "encoder.h"
#include "input.h"
#include "generated/ext-image-capture-source-v1.h"
#include "generated/ext-image-copy-capture-v1.h"
#include "generated/linux-dmabuf-v1.h"
#include "generated/virtual-keyboard-unstable-v1.h"
#include "generated/wlr-virtual-pointer-unstable-v1.h"
#include "../../proto/wire.h"

struct ww {
    struct wl_display *dpy;
    struct wl_registry *reg;
    struct wl_output *output;
    struct wl_seat *seat;
    struct ext_output_image_capture_source_manager_v1 *source_mgr;
    struct ext_image_copy_capture_manager_v1 *capture_mgr;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct zwp_virtual_keyboard_manager_v1 *kbmgr;
    struct zwlr_virtual_pointer_manager_v1 *ptrmgr;

    struct ext_image_capture_source_v1 *source;
    struct ext_image_copy_capture_session_v1 *session;
    struct ext_image_copy_capture_frame_v1 *frame;

    uint32_t width, height;
    bool session_ready, size_known;
    uint32_t fourcc; uint64_t modifier;

    struct gbm_device *gbm;
    struct gbm_bo *bo;
    struct wl_buffer *wl_buf;
    int dmabuf_fd;
    uint32_t stride;
    void *cpu_map;

    struct ww_enc *enc;
    struct ww_input *input;
    struct ww_shm *shm;
    bool use_shm;

    int video_fd;
    struct sockaddr_un video_addr;
    int ctrl_fd;

    uint32_t frame_id;
    uint64_t capture_ts_us;
    uint64_t last_frame_us;
    int fps;
    int qp;
    bool need_idr;
    uint64_t frames_sent, bytes_sent;
    bool first_send_logged;
    bool dump_once;
    /* 分段耗时统计（微秒） */
    uint64_t t_tick_us;
    uint64_t acc_cap_us, acc_conv_us, acc_enc_us, acc_n;
    const char *drm;
    double scale;
    uint32_t enc_w, enc_h;   /* 编码器当前尺寸，用于判断是否需要重建 */
    bool auto_adapt;         /* 自动升降档（用户手动锁定分辨率时关闭） */
    uint32_t def_max_w, def_max_h;
    int def_fps;
    const char *enc_kind;    /* "vaapi" 集显 | "nvenc" 独显 */
    const char *output_name; /* Hyprland 输出名：eDP-1（集显会话）或 HEADLESS-1（独显会话） */
    uint32_t max_w, max_h;   /* 协商上限：超过就按上限降采样，保证帧率 */
    /* 协商与自适应档位 */
    uint32_t req_w, req_h;   /* 客户端原始请求（物理像素） */
    double req_scale;
    int cur_preset, max_preset;
    uint64_t slow_since, fast_since;
};

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* CPU 读 dmabuf 前必须做缓存同步，否则读到的是陈旧/零数据（画面全黑的根因） */
static void dmabuf_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = { .flags = flags };
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

/* 一次性把当前映射的 ARGB 落成 PPM，用于验证抓到的像素是否正确 */
static void dump_ppm_once(struct ww *w);

static uint64_t now_us_(void) { return now_us(); }

/* ── 极小 JSON 取值（够用即可，避免引入解析库）───────────────── */
static const char *json_find(const char *s, const char *key)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    return strstr(s, pat);
}

/* 取 JSON 里的字符串值（够用即可） */
static const char *jstr(const char *s, const char *key, char *out, size_t n)
{
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(s, pat);
    if (!p) {
        return NULL;
    }
    p += strlen(pat);
    const char *q = strchr(p, '"');
    if (!q) {
        return NULL;
    }
    size_t len = (size_t)(q - p);
    if (len >= n) len = n - 1;
    memcpy(out, p, len);
    out[len] = 0;
    return out;
}

static double jnum(const char *s, const char *key, double def)
{
    const char *p = json_find(s, key);
    if (!p) return def;
    p += strlen(key) + 3;
    return strtod(p, NULL);
}

/* 分辨率档位：按客户端请求的物理像素就近取档，并用 scale 保持逻辑尺寸不变 */
static void apply_preset(struct ww *w, int pi)
{
    static const uint32_t PW[4] = { 1920, 2560, 3200, 3840 };
    if (pi < 0) pi = 0;
    if (pi > 3) pi = 3;
    if ((int)PW[pi] > (int)w->max_w) pi = 0;

    uint32_t pw = PW[pi];
    /* 桌面始终按 16:9 出图：客户端窗口比例只影响浏览器的信箱留黑，
       绝不据此改变桌面纵横比（曾算出 1920x3840 的竖屏） */
    uint32_t ph = (pw * 9 / 16) & ~1u;

    /* 逻辑宽度取自客户端 DPR 换算；窗口过小（<1280 逻辑）时按 1:1 处理 */
    double logical_w = (w->req_w && w->req_scale > 0.1)
                         ? (double)w->req_w / w->req_scale
                         : (double)pw;
    if (logical_w < 1280.0) {
        logical_w = (double)pw;   /* 小窗口不参与缩放推导 */
    }
    double sc = (double)pw / logical_w;
    if (sc < 1.0) sc = 1.0;
    if (sc > 2.0) sc = 2.0;

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "hyprctl keyword monitor %s,%ux%u@60,0x0,%.2f >/dev/null 2>&1", w->output_name, pw, ph, sc);
    if (system(cmd) == 0) {
        w->cur_preset = pi;
        w->scale = sc;
        fprintf(stderr, "[cap] 输出 %ux%u scale=%.2f（逻辑 %.0fx%.0f，档位%d）\n",
                pw, ph, sc, logical_w, logical_w * ph / pw, pi);
    }
}

static void handle_ctrl(struct ww *w, const char *msg)
{
    const char *t = strstr(msg, "\"t\":\"");
    if (!t) return;
    t += 5;
    char type[8] = {0};
    for (int i = 0; i < 7 && t[i] && t[i] != '"'; i++) type[i] = t[i];

    if (strcmp(type, "pm") == 0) {
        ww_input_motion_abs(w->input, jnum(msg, "x", 0), jnum(msg, "y", 0));
    } else if (strcmp(type, "pr") == 0) {
        ww_input_motion_rel(w->input, jnum(msg, "dx", 0), jnum(msg, "dy", 0));
    } else if (strcmp(type, "pb") == 0) {
        ww_input_button(w->input, (uint32_t)jnum(msg, "b", 1), jnum(msg, "d", 0) > 0);
    } else if (strcmp(type, "pw") == 0) {
        ww_input_axis(w->input, jnum(msg, "v", 0));
    } else if (strcmp(type, "kk") == 0) {
        ww_input_key(w->input, (uint32_t)jnum(msg, "k", 0), jnum(msg, "d", 0) > 0);
    } else if (strcmp(type, "qp") == 0) {
        w->qp = (int)jnum(msg, "v", w->qp);
        ww_enc_set_qp(w->enc, w->qp);
        fprintf(stderr, "[cap] QP -> %d\n", w->qp);
    } else if (strcmp(type, "fps") == 0) {
        int v = (int)jnum(msg, "v", 0);
        if (v == 0) {
            v = w->def_fps;   /* 0 = 自动：回到启动时的默认帧率 */
        }
        if (v > 0 && v <= 120 && w->shm) {
            ww_shm_set_fps(w->shm, v);
            fprintf(stderr, "[cap] 采集帧率设为 %d fps\n", v);
        }
    } else if (strcmp(type, "enc") == 0) {
        char v[16];
        if (jstr(msg, "v", v, sizeof(v)) && strcmp(v, w->enc_kind) != 0) {
            const char *kind = (!strcmp(v, "nvenc")) ? "nvenc" : "vaapi";
            fprintf(stderr, "[cap] 切换编码器 %s -> %s\n", w->enc_kind, kind);
            w->enc_kind = kind;
            /* 下一帧发现尺寸不符时会自动销毁重建 —— 这里把基准清掉即可 */
            w->enc_w = 0;
            w->enc_h = 0;
            w->need_idr = true;
        }
    } else if (strcmp(type, "kf") == 0) {
        w->need_idr = true;
    } else if (strcmp(type, "mode") == 0) {
        if (jnum(msg, "reset", 0) > 0) {
            /* 最后一个客户端断开：回到默认 1080p，避免桌面停留在小窗口尺寸 */
            w->req_w = 0;
            w->req_h = 0;
            w->req_scale = 1.0;
            w->cur_preset = 0;
            char cmd[192];
            snprintf(cmd, sizeof(cmd),
                     "hyprctl keyword monitor %s,1920x1080@60,0x0,1 >/dev/null 2>&1", w->output_name);
            if (system(cmd) == 0) {
                w->scale = 1.0;
                fprintf(stderr, "[cap] 无客户端，输出恢复 1920x1080@1x\n");
            }
            return;
        }

        int mw = (int)jnum(msg, "w", (double)w->width);
        int mh = (int)jnum(msg, "h", (double)w->height);
        double sc = jnum(msg, "scale", 1.0);

        /* lock=1：用户在客户端设置里手动选了分辨率，关掉自动升降档并放开上限 */
        if (jnum(msg, "lock", 0) > 0) {
            w->auto_adapt = false;
            w->max_w = (mw > 3840) ? 3840u : (mw < 1280 ? 1280u : (uint32_t)mw);
            w->max_h = (mh > 2160) ? 2160u : (mh < 720 ? 720u : (uint32_t)mh);
        } else {
            w->auto_adapt = true;
            w->max_w = w->def_max_w;
            w->max_h = w->def_max_h;
        }
        if (sc < 1.0) sc = 1.0;
        if (sc > 2.0) sc = 2.0;

        /* 只在“物理像素”上做档位量化：1920/2560/3200/3840 四档，
           绝不允许被任意小窗口拉到离谱的低分辨率（曾出现 824x1648，画面必然糊） */
        int want = 1920;
        if (mw >= 2560) want = 2560;
        if (mw >= 3200) want = 3200;
        if (mw >= 3840) want = 3840;
        if (want > (int)w->max_w) want = (int)w->max_w;
        if (want < 1920) want = 1920;

        w->req_w = (uint32_t)mw;
        w->req_h = (uint32_t)mh;
        w->req_scale = sc;
        w->max_preset = (int)(want / 640) - 3;   /* 1920->0 2560->1 3200->2 3840->3 */
        if (w->max_preset < 0) w->max_preset = 0;

        int pi = (int)(want / 640) - 3;
        if (pi < 0) pi = 0;
        apply_preset(w, pi);
    }
}

/* ── session ─────────────────────────────────────────────────── */
static void on_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *s,
                           uint32_t w, uint32_t h)
{
    (void)s;
    struct ww *ww = data;
    ww->width = w; ww->height = h; ww->size_known = true;
    fprintf(stderr, "[cap] buffer_size %ux%u\n", w, h);
}

static void on_shm_format(void *d, struct ext_image_copy_capture_session_v1 *s, uint32_t f)
{ (void)d; (void)s; (void)f; }
static void on_dmabuf_device(void *d, struct ext_image_copy_capture_session_v1 *s, struct wl_array *a)
{ (void)d; (void)s; (void)a; }

static void on_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *s,
                             uint32_t format, struct wl_array *mods)
{
    (void)s;
    struct ww *ww = data;
    const uint64_t *m = mods->data;
    size_t n = mods->size / sizeof(uint64_t);
    bool rgb = (format == DRM_FORMAT_ARGB8888 || format == DRM_FORMAT_XRGB8888 ||
                format == DRM_FORMAT_ABGR8888 || format == DRM_FORMAT_XBGR8888);
    if (!rgb) return;
    /* 优先 linear（modifier 0），否则取第一个 */
    if (n == 0) {
        if (!ww->fourcc) { ww->fourcc = format; ww->modifier = 0; }
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (m[i] == 0) { ww->fourcc = format; ww->modifier = 0; return; }
    }
    if (!ww->fourcc) { ww->fourcc = format; ww->modifier = m[0]; }
}

static void on_session_done(void *data, struct ext_image_copy_capture_session_v1 *s)
{ (void)s; ((struct ww *)data)->session_ready = true; }
static void on_session_stopped(void *data, struct ext_image_copy_capture_session_v1 *s)
{ (void)s; fprintf(stderr, "[cap] session stopped\n"); (void)data; }

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = on_buffer_size,
    .shm_format = on_shm_format,
    .dmabuf_device = on_dmabuf_device,
    .dmabuf_format = on_dmabuf_format,
    .done = on_session_done,
    .stopped = on_session_stopped,
};

/* ── 帧 ──────────────────────────────────────────────────────── */
static int submit_frame(struct ww *w);

static void dump_ppm_bytes(struct ww *w, const void *base, uint32_t stride)
{
    if (!base || !w->width || !w->height) {
        return;
    }
    FILE *f = fopen("/tmp/ww-capture.ppm", "wb");
    if (!f) {
        return;
    }
    fprintf(f, "P6\n%u %u\n255\n", w->width, w->height);
    const uint8_t *pix = base;
    uint8_t *line = malloc((size_t)w->width * 3);
    if (!line) {
        fclose(f);
        return;
    }
    for (uint32_t y = 0; y < w->height; y++) {
        const uint8_t *row = pix + (size_t)y * stride;
        for (uint32_t x = 0; x < w->width; x++) {
            line[x * 3 + 0] = row[x * 4 + 2];
            line[x * 3 + 1] = row[x * 4 + 1];
            line[x * 3 + 2] = row[x * 4 + 0];
        }
        fwrite(line, 1, (size_t)w->width * 3, f);
    }
    free(line);
    fclose(f);
    fprintf(stderr, "[cap] 已落盘 /tmp/ww-capture.ppm（%ux%u）\n", w->width, w->height);
}

static void send_packet(struct ww *w, const struct ww_nal_out *nal)
{
    /* 首个访问单元落盘，用于 ffprobe 校验码流合法性 */
    if (w->frames_sent == 0 && nal->len > 0) {
        FILE *f = fopen("/tmp/ww-first.h264", "wb");
        if (f) {
            fwrite(nal->data, 1, nal->len, f);
            fclose(f);
            fprintf(stderr, "[cap] 首个 AU 已落盘 %zu 字节\n", nal->len);
        }
    }

    const size_t chunk = WW_MAX_PAYLOAD;
    uint16_t total = (uint16_t)((nal->len + chunk - 1) / chunk);
    if (total == 0) total = 1;

    uint8_t pkt[WW_VID_HDR_SIZE + WW_MAX_PAYLOAD];
    for (uint16_t i = 0; i < total; i++) {
        size_t off = (size_t)i * chunk;
        size_t len = nal->len - off;
        if (len > chunk) len = chunk;

        struct ww_vid_frag_hdr h = {
            .version = WW_WIRE_VERSION,
            .flags = (uint8_t)((nal->keyframe ? WW_FRAG_KEY : 0) |
                               (i == 0 ? WW_FRAG_FIRST : 0) |
                               (i == total - 1 ? WW_FRAG_LAST : 0)),
            .stream_id = 0,
            .frame_id = w->frame_id,
            .frag_index = i,
            .frag_count = total,
            .payload_len = (uint32_t)len,
            .pts_us = (uint64_t)nal->pts_us,
        };
        memcpy(pkt, &h, sizeof(h));
        if (len) memcpy(pkt + sizeof(h), nal->data + off, len);

        ssize_t n = sendto(w->video_fd, pkt, sizeof(h) + len, MSG_DONTWAIT,
                           (struct sockaddr *)&w->video_addr, sizeof(w->video_addr));
        if (n < 0) {
            if (errno != EAGAIN && errno != ECONNREFUSED && errno != ENOENT) {
                static int logged;
                if (!logged) { fprintf(stderr, "[cap] sendto 失败: %s\n", strerror(errno)); logged = 1; }
            }
        } else {
            w->bytes_sent += (uint64_t)n;
            if (!w->first_send_logged) {
                w->first_send_logged = true;
                fprintf(stderr, "[cap] 首个分片已发送 %zd 字节 -> %s\n", n, w->video_addr.sun_path);
            }
        }
    }
    w->frame_id++;
    w->frames_sent++;
    w->last_frame_us = now_us();
}

static void on_frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *f)
{
    struct ww *w = data;
    ext_image_copy_capture_frame_v1_destroy(f);
    w->frame = NULL;

    /* shm 模式下帧由 screencopy 提供，ext-capture 仅用于协商尺寸 */
    if (w->use_shm) {
        return;
    }

    bool idr = w->need_idr;
    w->need_idr = false;

    int rc;
    if (w->cpu_map) {
        /* 关键：GPU 写入 dmabuf 后 CPU 才能读到有效像素 */
        dmabuf_sync(w->dmabuf_fd, DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ);
        if (w->dump_once) {
            dump_ppm_bytes(w, w->cpu_map, w->stride);
            w->dump_once = false;
        }
        rc = ww_enc_submit_cpu(w->enc, w->cpu_map, w->fourcc, w->width, w->height,
                               w->stride, (int64_t)w->capture_ts_us, idr);
        dmabuf_sync(w->dmabuf_fd, DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ);
    } else {
        rc = ww_enc_submit_dmabuf(w->enc, w->dmabuf_fd, w->fourcc, w->width, w->height,
                                  w->stride, w->modifier, (int64_t)w->capture_ts_us, idr);
    }
    if (rc != 0) {
        fprintf(stderr, "[cap] 编码提交失败\n");
    } else {
        struct ww_nal_out out;
        while (ww_enc_receive(w->enc, &out) == 1) {
            send_packet(w, &out);
        }
    }
    submit_frame(w);
}

static void on_frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *f, uint32_t reason)
{
    struct ww *w = data;
    fprintf(stderr, "[cap] frame failed reason=%u\n", reason);
    ext_image_copy_capture_frame_v1_destroy(f);
    w->frame = NULL;
    submit_frame(w);
}

static void on_frame_transform(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t t)
{ (void)d; (void)f; (void)t; }
static void on_frame_damage(void *d, struct ext_image_copy_capture_frame_v1 *f, int32_t x, int32_t y,
                            int32_t ww_, int32_t hh)
{ (void)d; (void)f; (void)x; (void)y; (void)ww_; (void)hh; }
static void on_frame_pts(void *d, struct ext_image_copy_capture_frame_v1 *f, uint32_t a, uint32_t b,
                         uint32_t c)
{ (void)d; (void)f; (void)a; (void)b; (void)c; }

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = on_frame_transform,
    .damage = on_frame_damage,
    .presentation_time = on_frame_pts,
    .ready = on_frame_ready,
    .failed = on_frame_failed,
};

static int submit_frame(struct ww *w)
{
    if (w->frame) return 0;
    w->frame = ext_image_copy_capture_session_v1_create_frame(w->session);
    if (!w->frame) return -1;
    ext_image_copy_capture_frame_v1_add_listener(w->frame, &frame_listener, w);
    ext_image_copy_capture_frame_v1_attach_buffer(w->frame, w->wl_buf);
    ext_image_copy_capture_frame_v1_damage_buffer(w->frame, 0, 0, w->width, w->height);
    w->capture_ts_us = now_us();
    ext_image_copy_capture_frame_v1_capture(w->frame);
    return 0;
}

static void on_buf_release(void *d, struct wl_buffer *b) { (void)d; (void)b; }
static const struct wl_buffer_listener buf_listener = { .release = on_buf_release };

/* ── registry ────────────────────────────────────────────────── */
static void on_global(void *data, struct wl_registry *r, uint32_t name, const char *iface,
                      uint32_t ver)
{
    struct ww *w = data;
    if (!strcmp(iface, wl_output_interface.name)) {
        if (!w->output) w->output = wl_registry_bind(r, name, &wl_output_interface, ver >= 4 ? 4 : ver);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        if (!w->seat) w->seat = wl_registry_bind(r, name, &wl_seat_interface, ver >= 5 ? 5 : ver);
    } else if (!strcmp(iface, ext_output_image_capture_source_manager_v1_interface.name)) {
        w->source_mgr = wl_registry_bind(r, name, &ext_output_image_capture_source_manager_v1_interface, 1);
    } else if (!strcmp(iface, ext_image_copy_capture_manager_v1_interface.name)) {
        w->capture_mgr = wl_registry_bind(r, name, &ext_image_copy_capture_manager_v1_interface, 1);
    } else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
        w->dmabuf = wl_registry_bind(r, name, &zwp_linux_dmabuf_v1_interface, ver >= 3 ? 3 : ver);
    } else if (!strcmp(iface, zwp_virtual_keyboard_manager_v1_interface.name)) {
        w->kbmgr = wl_registry_bind(r, name, &zwp_virtual_keyboard_manager_v1_interface, 1);
    } else if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name)) {
        w->ptrmgr = wl_registry_bind(r, name, &zwlr_virtual_pointer_manager_v1_interface, ver >= 2 ? 2 : 1);
    }
}
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_listener = { .global = on_global, .global_remove = on_global_remove };

/* ── shm 帧回调：编码 + 发送 ─────────────────────────────────── */
static void on_shm_frame(void *ud, const void *data, uint32_t stride,
                         uint32_t fw, uint32_t fh, uint64_t pts)
{
    struct ww *w = ud;
    uint64_t t_ready = now_us();
    if (w->t_tick_us && t_ready > w->t_tick_us) {
        w->acc_cap_us += t_ready - w->t_tick_us;
    }

    /* 分辨率变化（协商或手动切换）：以「编码器实际尺寸」为准判断，避免状态错位 */
    if (fw != w->enc_w || fh != w->enc_h) {
        fprintf(stderr, "[cap] 帧尺寸 %ux%u，编码器 %ux%u → 重建\n",
                fw, fh, w->enc_w, w->enc_h);
        if (w->enc) {
            ww_enc_destroy(w->enc);
            w->enc = NULL;
        }
        w->enc = ww_enc_create_ex(w->enc_kind, w->drm, (int)fw, (int)fh, w->fps, w->qp);
        if (!w->enc) {
            fprintf(stderr, "[cap] 编码器重建失败（%ux%u）\n", fw, fh);
            w->enc_w = w->enc_h = 0;
            return;
        }
        w->enc_w = fw;
        w->enc_h = fh;
        w->width = fw;
        w->height = fh;
        ww_input_set_geometry(w->input, (int)fw, (int)fh);
        w->need_idr = true;
    }

    w->capture_ts_us = pts;

    if (w->dump_once) {
        dump_ppm_bytes(w, data, stride);
        w->dump_once = false;
    }

    bool idr = w->need_idr;
    w->need_idr = false;

    if (ww_enc_submit_cpu(w->enc, data, w->fourcc, w->width, w->height, stride,
                          (int64_t)pts, idr) != 0) {
        static bool logged;
        if (!logged) {
            logged = true;
            fprintf(stderr, "[cap] submit 失败：frame=%ux%u enc=%p（尺寸不匹配或编码器为空）\n",
                    fw, fh, (void *)w->enc);
        }
        return;
    }
    uint64_t t_conv = now_us();
    struct ww_nal_out out;
    while (ww_enc_receive(w->enc, &out) == 1) {
        send_packet(w, &out);
    }
    uint64_t t_end = now_us();
    w->acc_conv_us += t_conv - t_ready;
    w->acc_enc_us += t_end - t_conv;
    w->acc_n++;
    w->last_frame_us = t_end;
}

/* ── main ────────────────────────────────────────────────────── */
static void mkaddr(struct sockaddr_un *a, const char *path)
{
    memset(a, 0, sizeof(*a));
    a->sun_family = AF_UNIX;
    snprintf(a->sun_path, sizeof(a->sun_path), "%s", path);
}

int main(int argc, char **argv)
{
    const char *drm = "/dev/dri/renderD129";
    const char *video_path = NULL;
    const char *ctrl_path = NULL;
    int qp = 20, fps = 60;   /* CQP 是唯一可调项：20 明显比默认值清晰，码率仍可接受 */
    bool dump_once = false;
    uint32_t max_w = 3200, max_h = 1800;   /* 默认上限：本机 4K 回读撑不住 30fps */
    const char *enc_kind = "vaapi";        /* 默认集显；--enc nvenc 走独显 */
    bool cap_dmabuf = false;               /* --capture dmabuf：走 ext-capture 的 dmabuf 通路 */
    const char *output_name = "eDP-1";     /* --output：Hyprland 输出名 */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--drm") && i + 1 < argc) drm = argv[++i];
        else if (!strcmp(argv[i], "--video") && i + 1 < argc) video_path = argv[++i];
        else if (!strcmp(argv[i], "--ctrl") && i + 1 < argc) ctrl_path = argv[++i];
        else if (!strcmp(argv[i], "--qp") && i + 1 < argc) qp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump")) dump_once = true;
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) cap_dmabuf = !strcmp(argv[++i], "dmabuf");
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) output_name = argv[++i];
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) {
            unsigned tw = 0, th = 0;
            if (sscanf(argv[++i], "%ux%u", &tw, &th) == 2) { max_w = tw; max_h = th; }
        }
    }
    if (!video_path || !ctrl_path) {
        fprintf(stderr, "用法: wwcap --video <sock> --ctrl <sock> [--drm node] [--qp N] [--fps N]\n");
        return 2;
    }

    /* hyprctl IPC 需要实例签名：从 XDG_RUNTIME_DIR/hypr 下取最新目录 */
    if (!getenv("HYPRLAND_INSTANCE_SIGNATURE")) {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        if (rt) {
            char path[512];
            snprintf(path, sizeof(path), "%s/hypr", rt);
            char cmd[640];
            snprintf(cmd, sizeof(cmd),
                     "ls -dt %s/*/ 2>/dev/null | head -1 | xargs -r -I{} basename {}", path);
            FILE *p = popen(cmd, "r");
            if (p) {
                char line[256] = {0};
                if (fgets(line, sizeof(line), p)) {
                    size_t n = strcspn(line, "\r\n");
                    line[n] = 0;
                    if (n > 0) {
                        setenv("HYPRLAND_INSTANCE_SIGNATURE", line, 1);
                        fprintf(stderr, "[cap] HYPRLAND_INSTANCE_SIGNATURE=%s\n", line);
                    }
                }
                pclose(p);
            }
        }
    }

    struct ww w;
    memset(&w, 0, sizeof(w));
    w.fps = fps;
    w.qp = qp;
    w.drm = drm;
    w.scale = 1.0;
    w.max_w = max_w;
    w.max_h = max_h;
    w.def_max_w = max_w;
    w.def_max_h = max_h;
    w.auto_adapt = true;
    w.def_fps = fps;
    w.enc_kind = enc_kind;
    w.output_name = output_name;
    w.dump_once = dump_once;

    w.dpy = wl_display_connect(NULL);
    if (!w.dpy) { fprintf(stderr, "无法连接 Wayland\n"); return 1; }

    w.reg = wl_display_get_registry(w.dpy);
    wl_registry_add_listener(w.reg, &reg_listener, &w);
    wl_display_roundtrip(w.dpy);
    if (!w.source_mgr || !w.capture_mgr || !w.dmabuf || !w.output) {
        fprintf(stderr, "缺少必需的 Wayland 全局对象\n");
        return 1;
    }

    /* gbm（与 compositor 同卡） */
    const char *nodes[] = { "/dev/dri/card1", "/dev/dri/renderD129", NULL };
    for (int i = 0; nodes[i] && !w.gbm; i++) {
        int fd = open(nodes[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        w.gbm = gbm_create_device(fd);
        if (!w.gbm) close(fd);
    }
    if (!w.gbm) { fprintf(stderr, "gbm 初始化失败\n"); return 1; }

    w.source = ext_output_image_capture_source_manager_v1_create_source(w.source_mgr, w.output);
    w.session = ext_image_copy_capture_manager_v1_create_session(w.capture_mgr, w.source, 0);
    ext_image_copy_capture_session_v1_add_listener(w.session, &session_listener, &w);
    while (!w.session_ready && wl_display_dispatch(w.dpy) != -1) {}
    if (!w.size_known || !w.fourcc) { fprintf(stderr, "session 协商失败\n"); return 1; }

    uint32_t flags = (w.modifier == 0) ? GBM_BO_USE_LINEAR : 0;
    w.bo = gbm_bo_create(w.gbm, w.width, w.height, w.fourcc, flags);
    if (!w.bo) { fprintf(stderr, "gbm_bo_create 失败\n"); return 1; }
    w.stride = gbm_bo_get_stride(w.bo);
    w.dmabuf_fd = gbm_bo_get_fd(w.bo);
    if (w.dmabuf_fd < 0) { fprintf(stderr, "gbm_bo_get_fd 失败\n"); return 1; }

    /* 常驻 CPU 映射：VPP 不可用时用它做色彩转换（不 unmap，规避 mesa 26 的 unmap 崩溃） */
    if (w.modifier == 0) {
        uint32_t mstride = 0;
        w.cpu_map = gbm_bo_map(w.bo, 0, 0, w.width, w.height,
                               GBM_BO_TRANSFER_READ | GBM_BO_TRANSFER_WRITE, &mstride, &w.cpu_map);
        if (w.cpu_map) {
            w.stride = mstride;
            fprintf(stderr, "[cap] CPU 映射就绪 stride=%u\n", mstride);
        } else {
            fprintf(stderr, "[cap] CPU 映射失败，走 dmabuf 路径\n");
        }
    }

    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(w.dmabuf);
    zwp_linux_buffer_params_v1_add(params, w.dmabuf_fd, 0, 0, w.stride,
                                   (uint32_t)(w.modifier >> 32), (uint32_t)(w.modifier & 0xffffffffu));
    w.wl_buf = zwp_linux_buffer_params_v1_create_immed(params, w.width, w.height, w.fourcc, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    if (!w.wl_buf) { fprintf(stderr, "wl_buffer 创建失败\n"); return 1; }
    wl_buffer_add_listener(w.wl_buf, &buf_listener, &w);

    fprintf(stderr, "[cap] 抓帧目标 %ux%u fourcc=0x%x modifier=0x%llx stride=%u\n",
            w.width, w.height, w.fourcc, (unsigned long long)w.modifier, w.stride);

    w.enc = ww_enc_create(drm, (int)w.width, (int)w.height, fps, qp);
    if (!w.enc) { fprintf(stderr, "编码器初始化失败\n"); return 1; }
    w.enc_w = w.width;
    w.enc_h = w.height;

    w.input = ww_input_create(w.dpy, w.seat);
    if (w.input) {
        ww_input_attach(w.input, w.kbmgr, w.ptrmgr, w.seat);
        ww_input_set_geometry(w.input, (int)w.width, (int)w.height);
        FILE *cp = popen("hyprctl cursorpos 2>/dev/null", "r");
        if (cp) {
            char line[64] = {0};
            if (fgets(line, sizeof(line), cp)) {
                int px = 0, py = 0;
                if (sscanf(line, "%d, %d", &px, &py) == 2) {
                    ww_input_set_position(w.input, px, py);
                    fprintf(stderr, "[input] 初始光标位置 %d,%d\n", px, py);
                }
            }
            pclose(cp);
        }
    }

    /* 默认走 screencopy+shm；--capture dmabuf 走 ext-image-copy-capture 的 dmabuf 通路 */
    w.use_shm = !cap_dmabuf;
    if (!cap_dmabuf) {
        w.shm = ww_shm_create(w.dpy, w.output, fps, on_shm_frame, &w);
        if (!w.shm) {
            fprintf(stderr, "[cap] screencopy 不可用，回退 ext-image-copy-capture\n");
            w.use_shm = false;
        }
    }

    /* IPC */
    w.video_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    mkaddr(&w.video_addr, video_path);

    w.ctrl_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un ctrl_addr;
    mkaddr(&ctrl_addr, ctrl_path);
    unlink(ctrl_path);
    if (bind(w.ctrl_fd, (struct sockaddr *)&ctrl_addr, sizeof(ctrl_addr)) < 0) {
        fprintf(stderr, "[cap] 绑定 ctrl socket 失败: %s\n", strerror(errno));
    }

    if (submit_frame(&w) != 0) { fprintf(stderr, "首帧提交失败\n"); return 1; }
    w.last_frame_us = now_us();
    fprintf(stderr, "[cap] 运行中\n");

    struct pollfd pfd[2];
    pfd[0].fd = wl_display_get_fd(w.dpy); pfd[0].events = POLLIN;
    pfd[1].fd = w.ctrl_fd; pfd[1].events = POLLIN;

    uint64_t stat_t = now_us();
    uint64_t stat_frames = 0;

    for (;;) {
        wl_display_flush(w.dpy);
        /* 轮询粒度要远小于帧间隔，否则抓帧节拍会被 poll 超时量化掉 */
        if (poll(pfd, 2, 1) < 0 && errno != EINTR) break;

        if (pfd[1].revents & POLLIN) {
            char buf[2048];
            for (;;) {
                ssize_t n = recv(w.ctrl_fd, buf, sizeof(buf) - 1, 0);
                if (n <= 0) break;
                buf[n] = 0;   /* 按实际长度封尾，避免读到上一条报文残留 */
                handle_ctrl(&w, buf);
            }
        }

        if (pfd[0].revents & POLLIN) {
            if (wl_display_dispatch(w.dpy) == -1) break;
        }

        uint64_t t = now_us();

        /* shm 模式按固定帧率主动抓帧；ext 模式靠 damage + 空闲强制重绘 */
        if (w.use_shm) {
            /* 只在真正发起抓帧时打点，否则 cap 统计会被每轮循环重置 */
            if (!ww_shm_pending(w.shm)) {
                w.t_tick_us = now_us();
            }
            ww_shm_tick(w.shm);
        } else if (w.last_frame_us && t - w.last_frame_us > 800000) {
            system("hyprctl dispatch forcerendererreload >/dev/null 2>&1");
            w.last_frame_us = t;
        }

        if (t - stat_t >= 1000000) {
            double n = w.acc_n ? (double)w.acc_n : 1.0;
            double ec = 0, eu = 0, es = 0;
            if (w.enc) {
                ww_enc_take_stats(w.enc, &ec, &eu, &es);
            }
            fprintf(stderr, "[enc] 转换 %.1fms 上传 %.1fms 提交 %.1fms\n", ec, eu, es);

            /* 清晰优先的自适应：帧率不足就降一档，余量充足再升回去 */
            if (w.req_w && w.auto_adapt) {
                double fps_now = (double)(w.frames_sent - stat_frames) * 1e6 / (double)(t - stat_t);
                if (fps_now < 26.0 && w.cur_preset > 0) {
                    if (!w.slow_since) {
                        w.slow_since = t;
                    } else if (t - w.slow_since > 2000000) {
                        fprintf(stderr, "[cap] 帧率 %.0f 不足，降档\n", fps_now);
                        apply_preset(&w, w.cur_preset - 1);
                        w.slow_since = 0;
                    }
                    w.fast_since = 0;
                } else if (fps_now > 46.0 && w.cur_preset < w.max_preset) {
                    if (!w.fast_since) {
                        w.fast_since = t;
                    } else if (t - w.fast_since > 6000000) {
                        fprintf(stderr, "[cap] 帧率 %.0f 有余量，升档\n", fps_now);
                        apply_preset(&w, w.cur_preset + 1);
                        w.fast_since = 0;
                    }
                    w.slow_since = 0;
                } else {
                    w.slow_since = 0;
                    w.fast_since = 0;
                }
            }
            fprintf(stderr, "[cap] %.1f fps, %.2f Mbps, frames=%llu | cap %.1fms conv %.1fms enc %.1fms\n",
                    (double)(w.frames_sent - stat_frames) * 1e6 / (double)(t - stat_t),
                    (double)w.bytes_sent * 8.0 / (double)(t - stat_t),
                    (unsigned long long)w.frames_sent,
                    w.shm ? ww_shm_take_cap_ms(w.shm) : 0.0,
                    (double)w.acc_conv_us / n / 1000.0,
                    (double)w.acc_enc_us / n / 1000.0);
            w.acc_cap_us = w.acc_conv_us = w.acc_enc_us = w.acc_n = 0;
            stat_t = t;
            stat_frames = w.frames_sent;
            w.bytes_sent = 0;
        }
    }

    ww_enc_destroy(w.enc);
    ww_input_destroy(w.input);
    wl_display_disconnect(w.dpy);
    return 0;
}
