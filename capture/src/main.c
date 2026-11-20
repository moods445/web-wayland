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
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <sys/file.h>
#include <sys/stat.h>
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
#ifdef HAVE_SC_CAPTURE
#include "capture_sc.h"
#endif
#include "audio.h"
#include "encoder.h"
#include "input.h"
#include "generated/ext-image-capture-source-v1.h"
#include "generated/ext-image-copy-capture-v1.h"
#include "generated/linux-dmabuf-v1.h"
#include "generated/virtual-keyboard-unstable-v1.h"
#include "generated/wlr-virtual-pointer-unstable-v1.h"
#include "../../proto/wire.h"

/* 输出探测：Wayland 里 wl_output 的全局顺序不等于 Hyprland 的输出顺序，
   eDP-1 可能排在前面（且模式可能是 0x0）。这里按 --output 名字精确挑一个。 */
#define WW_MAX_OUT 8
struct out_probe {
    struct wl_output *o;
    struct ww *w;
    uint32_t gname;
    char name[64];
};

struct ww {
    struct wl_display *dpy;
    struct wl_registry *reg;
    struct wl_output *output;
    struct out_probe probes[WW_MAX_OUT];
    int n_probes;
    /* 新输出绑定后需要一次 roundtrip 才能拿到初始 mode/scale */
    bool layout_dirty;
    /* 上一次同步给输入模块的布局，用于避免重复日志 */
    int in_ox, in_oy, in_ow, in_oh, in_lw, in_lh;
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
#ifdef HAVE_SC_CAPTURE
    struct ww_sc *sc;      /* --capture sc：screencopy dmabuf → EGL/GL → NV12 */
#endif

    int video_fd;
    struct sockaddr_un video_addr;
    int ctrl_fd;

    /* 音频：--audio 未给出时为 NULL，整条音频链路保持关闭 */
    struct ww_audio *aud;
    int audio_fd;

    uint32_t frame_id;
    uint64_t capture_ts_us;
    uint64_t last_frame_us;
    int fps;
    int qp;
    /* 目标码率（bps）：0 = 关闭，走 CQP。cbr_avail 是当前编码器的能力，
       不支持时 bitrate_bps 会被清零（否则 UI 会以为设置生效了） */
    int64_t bitrate_bps;
    bool cbr_avail;
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
    uint64_t enc_retry_at;   /* 编码器创建失败后的重试时间（避免逐帧重建） */
    bool preset_applied;     /* 是否已经成功下发过一次 hyprctl 输出模式 */
    uint32_t tgt_w, tgt_h;   /* 最近一次期望的输出尺寸，用于校验实际抓帧尺寸 */
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
    /* 串流策略：只调 QP 与目标帧率，永不改动分辨率 */
    int strategy;
    /* 色度采样：pref 是用户偏好，chroma444 是当前实际生效值 */
    bool chroma_pref, chroma444, chroma444_avail;
    /* 休眠：没有客户端会话时停抓帧/编码并释放编码器，进程常驻等唤醒。
       网关在会话建立/最后一个会话结束时发 {"t":"active","v":1|0}。 */
    bool active;
    bool suspended;
    uint64_t idle_since_us;   /* 最后一个客户端离开的时刻；0 表示当前有会话 */

    /* ── 输入/画面追踪（排障用）────────────────────────────────
       鼠标"卡"这类问题靠肉眼说不清，这里把每条输入的「收到→注入完成」耗时、
       每条出站帧的「帧号/pts/字节/发送时刻」记进环形缓冲，每 5 秒把**新增**记录
       追加到 /tmp/wwcap-trace.jsonl（一行一条，可 tail -f / jq），2MB 轮转。
       追加而非整体重写：文件任何时刻都是完整可读的行式日志，也不会有覆盖空窗。 */
    struct ww_tr_in {
        uint64_t t_recv, t_done;
        double ts_client;      /* 客户端 performance.now()，用于对齐两边时钟 */
        double x, y;           /* pm 是归一化坐标（0~1），必须是浮点，截断会全变 0 */
        uint8_t kind;          /* WW_TR_* */
    } tr_in[1024];
    uint32_t tr_in_head;       /* 累计写入条数（环形下标 = head % N） */
    uint32_t tr_in_flushed;    /* 已落盘的累计条数 */
    struct ww_tr_frm {
        uint64_t t_send;
        int64_t pts;
        uint32_t bytes, id;
        uint8_t key;
        /* 该帧里画进去的光标位置（归一化 0~1）。
           这是判定"鼠标卡不卡"的关键：客户端拿它和自己当时的指针位置对比，
           就能算出"画面上看到的光标比手落后了多少"（按坐标匹配，不依赖对钟）。 */
        float cx, cy;
    } tr_frm[512];
    uint32_t tr_frm_head;
    uint32_t tr_frm_flushed;
    /* 最近一次注入的指针归一化坐标（pm 消息带来的） */
    double cur_x, cur_y;
    bool cur_valid;
    uint64_t tr_flush_at;
    bool tr_dirty;
};

enum { WW_TR_PM = 0, WW_TR_PR, WW_TR_PB, WW_TR_PW, WW_TR_KK };

/* 追踪接口（定义在下面，handle_ctrl 先用） */
static void tr_input(struct ww *w, uint8_t kind, double x, double y,
                     double ts_client, uint64_t t_recv);
static void trace_flush(struct ww *w);

enum { WW_STRAT_BALANCE = 0, WW_STRAT_SMOOTH = 1, WW_STRAT_QUALITY = 2, WW_STRAT_CUSTOM = 3 };

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* 收到 SIGTERM 时先走正常清理（销毁虚拟键盘/指针），
   否则进程被直接杀掉，Hyprland 可能因设备突然消失而崩溃。 */
static volatile sig_atomic_t g_stop;
static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

/* CPU 读 dmabuf 前必须做缓存同步，否则读到的是陈旧/零数据（画面全黑的根因） */
static void dmabuf_sync(int fd, uint64_t flags)
{
    struct dma_buf_sync sync = { .flags = flags };
    ioctl(fd, DMA_BUF_IOCTL_SYNC, &sync);
}

/* 一次性把当前映射的 ARGB 落成 PPM，用于验证抓到的像素是否正确 */
static void dump_ppm_once(struct ww *w);

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
    /* 相同档位重复下发会让 hyprctl 阻塞输入处理，并触发一次无谓的模式切换 */
    double ds = sc - w->scale;
    if (ds < 0) ds = -ds;
    if (w->preset_applied && pi == w->cur_preset && ds < 0.01) {
        return;
    }
    if (system(cmd) == 0) {
        w->cur_preset = pi;
        w->scale = sc;
        w->preset_applied = true;
        w->tgt_w = pw;
        w->tgt_h = ph;
        fprintf(stderr, "[cap] 输出 %ux%u scale=%.2f（逻辑 %.0fx%.0f，档位%d）\n",
                pw, ph, sc, logical_w, logical_w * ph / pw, pi);
    }
}

/* ── 目标机扬声器静音（客户端「串流开始时静音目标机」开关）────────
   走 wpctl，和 config/hypr/hyprland.conf 里的音量快捷键同一套语义：
   项目本身不解析音量，只把用户意图转给 PipeWire。原状态只在第一次
   静音前记录，会话结束/进程退出时恢复，避免把桌面永久静音。 */
static int g_sink_saved_mute = -1;

static void sink_mute_apply(int on)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "wpctl set-mute @DEFAULT_AUDIO_SINK@ %d >/dev/null 2>&1", on ? 1 : 0);
    if (system(cmd) != 0) {
        fprintf(stderr, "[aud] wpctl 执行失败（wpctl 不可用？）\n");
    }
}

static void sink_mute_set(int on)
{
    if (g_sink_saved_mute < 0) {
        FILE *p = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
        if (p) {
            char buf[256] = {0};
            if (fgets(buf, sizeof(buf), p)) {
                g_sink_saved_mute = strstr(buf, "MUTED") ? 1 : 0;
            }
            pclose(p);
        }
        if (g_sink_saved_mute < 0) {
            g_sink_saved_mute = 0;   /* 读不到就按「未静音」处理，恢复时不会误静音 */
        }
    }
    sink_mute_apply(on);
    fprintf(stderr, "[aud] 目标机扬声器 -> %s（原状态 %s）\n",
            on ? "静音" : "未静音", g_sink_saved_mute ? "静音" : "未静音");
}

static void sink_mute_restore(void)
{
    if (g_sink_saved_mute < 0) {
        return;
    }
    sink_mute_apply(g_sink_saved_mute);
    fprintf(stderr, "[aud] 目标机扬声器已恢复原状态（%s）\n",
            g_sink_saved_mute ? "静音" : "未静音");
    g_sink_saved_mute = -1;
}

/* ── 串流策略与色度采样 ───────────────────────────────────────── */

static int strategy_preset_qp(int s)
{
    switch (s) {
    case WW_STRAT_SMOOTH:  return 26;   /* 24~30 之间按实测帧率浮动 */
    case WW_STRAT_QUALITY: return 16;
    case WW_STRAT_BALANCE: return 20;
    default:               return -1;   /* 自定义：不动 QP */
    }
}

/* 供网关 /capabilities 读取：客户端据此置灰 4:4:4 与目标码率开关。
   rc = "cbr" 表示可按目标码率编码；"cqp" 表示码率只能随画面浮动。 */
static void caps_write(struct ww *w)
{
    const char *path = getenv("WW_CAPS_PATH");
    if (!path || !*path) {
        path = "/tmp/wwcap-caps.json";
    }
    FILE *f = fopen(path, "w");
    if (!f) {
        return;
    }
    /* cursor=local：服务端用全透明光标主题（XCURSOR_THEME=ww-invisible）让合成器
       不把光标画进画面，客户端自己画一个——这样光标延迟是 0，而不是视频的两三帧。
       WW_CURSOR=server 可回退成"光标在视频里"的老行为。 */
    const char *cur = getenv("WW_CURSOR");
    const char *cur_mode = (cur && !strcmp(cur, "server")) ? "server" : "local";
    fprintf(f, "{\"chroma444\":%d,\"encoder\":\"%s\",\"rc\":\"%s\",\"bitrate_bps\":%lld,\"cursor\":\"%s\"}\n",
            w->chroma444_avail ? 1 : 0, w->enc_kind ? w->enc_kind : "",
            w->cbr_avail ? "cbr" : "cqp", (long long)w->bitrate_bps, cur_mode);
    fclose(f);
}

/* 改动编码参数（QP / 色度）后立刻重建，下一帧用新参数并强制 IDR */
static void encoder_restart(struct ww *w)
{
    if (w->enc) {
        ww_enc_destroy(w->enc);
        w->enc = NULL;
    }
    w->enc_w = 0;
    w->enc_h = 0;
    w->enc_retry_at = 0;
    w->need_idr = true;
}

/* 探测当前编码器的 4:4:4 能力，并让实际生效值跟随用户偏好 */
static void chroma_refresh(struct ww *w)
{
    bool avail = ww_enc_probe_444(w->enc_kind, w->drm);
    bool want = w->chroma_pref && avail;

    if (avail != w->chroma444_avail) {
        fprintf(stderr, "[enc] 4:4:4 %s（编码器 %s）\n",
                avail ? "可用" : "不可用", w->enc_kind ? w->enc_kind : "?");
    }
    w->chroma444_avail = avail;
    if (want != w->chroma444) {
        w->chroma444 = want;
        fprintf(stderr, "[enc] 色度采样 -> %s\n", want ? "4:4:4" : "4:2:0");
        encoder_restart(w);
    }
    caps_write(w);
}

/* 目标码率能力刷新：换编码器（vaapi ↔ nvenc）后必须重新探测，
   否则切到集显时客户端仍以为能设码率。 */
static void bitrate_refresh(struct ww *w)
{
    bool avail = ww_enc_probe_cbr(w->enc_kind, w->drm);
    if (avail != w->cbr_avail) {
        fprintf(stderr, "[enc] 目标码率（CBR）%s（编码器 %s）\n",
                avail ? "可用" : "不可用，仅 CQP", w->enc_kind ? w->enc_kind : "?");
    }
    w->cbr_avail = avail;
    if (!avail && w->bitrate_bps) {
        fprintf(stderr, "[enc] 目标码率 %.1f Mbps 失效，回退 QP %d\n",
                (double)w->bitrate_bps / 1e6, w->qp);
        w->bitrate_bps = 0;
    }
    caps_write(w);
}

/* 真实分辨率下 4:4:4 打不开（探测只试了 256x256）时回退 4:2:0，避免画面直接卡死 */
static bool encoder_fallback_420(struct ww *w)
{
    if (!w->chroma444) {
        return false;
    }
    fprintf(stderr, "[enc] 4:4:4 编码器创建失败，自动回退 4:2:0\n");
    w->chroma444 = false;
    w->chroma444_avail = false;
    caps_write(w);
    return true;
}

/* 策略只改 QP 与目标帧率，分辨率由客户端下拉单独决定 */
static void apply_strategy(struct ww *w, int s)
{
    if (s < WW_STRAT_BALANCE || s > WW_STRAT_CUSTOM) {
        return;
    }
    w->strategy = s;

    int qp = strategy_preset_qp(s);
    if (qp > 0) {
        w->qp = qp;
    }

    if (s == WW_STRAT_SMOOTH && w->fps != 30 && w->fps != 60) {
        w->fps = 60;
        if (w->shm) {
            ww_shm_set_fps(w->shm, 60);
        }
#ifdef HAVE_SC_CAPTURE
        if (w->sc) {
            ww_sc_set_fps(w->sc, 60);
        }
#endif
    }

    if (qp > 0) {
        /* NVENC 的 constqp 打开后不保证动态生效，重建最稳 */
        encoder_restart(w);
        fprintf(stderr, "[cap] 策略 -> %s（QP %d，目标 %d fps）\n",
                s == WW_STRAT_SMOOTH ? "流畅优先"
                : s == WW_STRAT_QUALITY ? "画质优先" : "平衡",
                w->qp, w->fps);
    } else {
        fprintf(stderr, "[cap] 策略 -> 自定义（QP %d）\n", w->qp);
    }
}

static void handle_ctrl(struct ww *w, const char *msg)
{
    /* 输入追踪的起点：消息从网关到达 wwcap 的时刻（决定上游延迟能否被观测到） */
    uint64_t t_recv = now_us();
    const char *t = strstr(msg, "\"t\":\"");
    if (!t) return;
    t += 5;
    char type[32] = {0};
    for (int i = 0; i < 31 && t[i] && t[i] != '"'; i++) type[i] = t[i];

    if (strcmp(type, "pm") == 0) {
        ww_input_motion_abs(w->input, jnum(msg, "x", 0), jnum(msg, "y", 0));
        tr_input(w, WW_TR_PM, jnum(msg, "x", 0), jnum(msg, "y", 0), jnum(msg, "ts", 0), t_recv);
    } else if (strcmp(type, "pr") == 0) {
        ww_input_motion_rel(w->input, jnum(msg, "dx", 0), jnum(msg, "dy", 0));
        tr_input(w, WW_TR_PR, jnum(msg, "dx", 0), jnum(msg, "dy", 0), jnum(msg, "ts", 0), t_recv);
    } else if (strcmp(type, "pb") == 0) {
        ww_input_button(w->input, (uint32_t)jnum(msg, "b", 1), jnum(msg, "d", 0) > 0);
        tr_input(w, WW_TR_PB, jnum(msg, "b", 1), jnum(msg, "d", 0), jnum(msg, "ts", 0), t_recv);
    } else if (strcmp(type, "pw") == 0) {
        ww_input_axis(w->input, jnum(msg, "v", 0));
        tr_input(w, WW_TR_PW, jnum(msg, "v", 0), 0, jnum(msg, "ts", 0), t_recv);
    } else if (strcmp(type, "kk") == 0) {
        ww_input_key(w->input, (uint32_t)jnum(msg, "k", 0), jnum(msg, "d", 0) > 0);
        tr_input(w, WW_TR_KK, jnum(msg, "k", 0), jnum(msg, "d", 0), jnum(msg, "ts", 0), t_recv);
    } else if (strcmp(type, "trace") == 0) {
        /* 客户端要求立刻落盘（导出一份"当前状态"的追踪） */
        trace_flush(w);
    } else if (strcmp(type, "qp") == 0) {
        /* 手动调 QP -> 进入「自定义」，策略不再覆盖 */
        w->strategy = WW_STRAT_CUSTOM;
        w->qp = (int)jnum(msg, "v", w->qp);
        ww_enc_set_qp(w->enc, w->qp);
        fprintf(stderr, "[cap] QP -> %d（自定义）\n", w->qp);
    } else if (strcmp(type, "bitrate") == 0) {
        /* 目标码率（bps）：0 = 关闭，回到 QP 控制。
           当前编码器只支持 CQP 时直接忽略，避免前端以为设置生效。 */
        int64_t v = (int64_t)jnum(msg, "v", 0);
        if (v < 0) v = 0;
        if (v > 200000000) v = 200000000;
        if (v > 0 && !w->cbr_avail) {
            fprintf(stderr, "[cap] 目标码率 %.1f Mbps 被忽略：当前编码器只支持 CQP\n",
                    (double)v / 1e6);
            v = 0;
        }
        if (v != w->bitrate_bps) {
            w->bitrate_bps = v;
            if (w->enc) {
                /* 码控模式在 avcodec_open2 时确定：CQP ↔ CBR 切换必须重建 */
                if ((v > 0) != ww_enc_is_cbr(w->enc)) {
                    encoder_restart(w);
                } else {
                    ww_enc_set_bitrate(w->enc, v);
                    w->need_idr = true;
                }
            }
            if (v > 0) {
                fprintf(stderr, "[cap] 目标码率 -> %.1f Mbps\n", (double)v / 1e6);
            } else {
                fprintf(stderr, "[cap] 目标码率 -> 关闭（QP %d 控制）\n", w->qp);
            }
            caps_write(w);
        }
    } else if (strcmp(type, "strategy") == 0) {
        apply_strategy(w, (int)jnum(msg, "v", WW_STRAT_BALANCE));
    } else if (strcmp(type, "chroma") == 0) {
        w->chroma_pref = jnum(msg, "v", 0) > 0;
        chroma_refresh(w);
    } else if (strcmp(type, "active") == 0) {
        /* 会话存续通知：没人看就不必抓帧编码（30 秒后进入休眠，见主循环） */
        bool on = jnum(msg, "v", 0) > 0;
        if (on != w->active) {
            w->active = on;
            w->idle_since_us = on ? 0 : now_us();
            fprintf(stderr, "[cap] 客户端会话%s\n", on ? "接入" : "全部断开");
        }
    } else if (strcmp(type, "fps") == 0) {
        int v = (int)jnum(msg, "v", 0);
        if (v == 0) {
            v = w->def_fps;   /* 0 = 自动：回到启动时的默认帧率 */
        }
        if (v > 0 && v <= 120 && w->shm) {
            ww_shm_set_fps(w->shm, v);
            w->fps = v;   /* 自适应升降档要按用户选择的目标帧率判断 */
            fprintf(stderr, "[cap] 采集帧率设为 %d fps\n", v);
        }
#ifdef HAVE_SC_CAPTURE
        if (v > 0 && v <= 120 && w->sc) {
            ww_sc_set_fps(w->sc, v);
            w->fps = v;
            fprintf(stderr, "[cap] 采集帧率设为 %d fps\n", v);
        }
#endif
    } else if (strcmp(type, "keysreset") == 0) {
        /* 客户端断开/刷新：补发全部释放，避免 SUPER/CTRL 卡死在桌面 */
        ww_input_reset(w->input);
    } else if (strcmp(type, "astream") == 0) {
        /* 客户端/网关声明是否需要下行音频：没人听就停采集，别空跑编码器 */
        bool rx = jnum(msg, "rx", 0) > 0;
        if (w->aud) {
            ww_audio_set_enabled(w->aud, rx);
            fprintf(stderr, "[aud] 下行音频 %s\n", rx ? "开启" : "关闭");
        }
    } else if (strcmp(type, "ared") == 0) {
        /* 冗余档位：0 关闭，>=1 每包带上一帧（网关按客户端丢包反馈下调/上调） */
        if (w->aud) {
            int n = (int)jnum(msg, "n", 1);
            ww_audio_set_redundancy(w->aud, n);
            fprintf(stderr, "[aud] RED 冗余 -> %d\n", n > 0 ? 1 : 0);
        }
    } else if (strcmp(type, "mute") == 0) {
        sink_mute_set(jnum(msg, "on", 0) > 0);
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
            /* 换编码器会改变 4:4:4 与目标码率能力：不支持时自动回退 */
            chroma_refresh(w);
            bitrate_refresh(w);
            /* 能力变化后按新编码器重建，别等下一帧才发现 */
            encoder_restart(w);
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
                w->cur_preset = 0;
                w->preset_applied = true;
                w->tgt_w = 1920;
                w->tgt_h = 1080;
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
    if (ww->tgt_w && (w != ww->tgt_w || h != ww->tgt_h)) {
        fprintf(stderr, "[cap] 警告：实际输出 %ux%u ≠ 期望 %ux%u（hyprctl 可能拒绝了该模式）\n",
                w, h, ww->tgt_w, ww->tgt_h);
    }
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

/* ── 输入/画面追踪 ──────────────────────────────────────────── */
#define WW_TR_IN_N ((uint32_t)(sizeof(((struct ww *)0)->tr_in) / sizeof(((struct ww *)0)->tr_in[0])))
#define WW_TR_FR_N ((uint32_t)(sizeof(((struct ww *)0)->tr_frm) / sizeof(((struct ww *)0)->tr_frm[0])))

/* 记录一条输入：t_recv 由调用方在收到消息时取，t_done 在注入返回后取 */
static void tr_input(struct ww *w, uint8_t kind, double x, double y,
                     double ts_client, uint64_t t_recv)
{
    if (kind == WW_TR_PM) {
        /* 记住最后一次注入的归一化坐标：下一帧里画进去的光标就在这个位置 */
        w->cur_x = x;
        w->cur_y = y;
        w->cur_valid = true;
    }
    struct ww_tr_in *e = &w->tr_in[w->tr_in_head % WW_TR_IN_N];
    e->t_recv = t_recv;
    e->t_done = now_us();
    e->ts_client = ts_client;
    e->x = x;
    e->y = y;
    e->kind = kind;
    w->tr_in_head++;
    w->tr_dirty = true;
}

/* 追加落盘（5 秒一次，或收到 trace 控制消息时立刻写）：
   一行一条记录（type=in / type=frm），文件是纯追加的，2MB 轮转成 .1。
   只写"上次落盘之后新增"的记录；环形缓冲被覆盖时（落后超过容量）从最旧可读位置开始。 */
static void trace_flush(struct ww *w)
{
    if (!w->tr_dirty) {
        return;
    }
    w->tr_dirty = false;
    const char *path = getenv("WW_TRACE_PATH");
    if (!path || !*path) {
        path = "/tmp/wwcap-trace.jsonl";
    }
    const size_t max_bytes = 2 * 1024 * 1024;

    struct stat st;
    if (stat(path, &st) == 0 && (size_t)st.st_size > max_bytes) {
        char bak[PATH_MAX];
        snprintf(bak, sizeof(bak), "%s.1", path);
        rename(path, bak);   /* 只保留一份旧档 */
    }

    FILE *f = fopen(path, "a");
    if (!f) {
        return;
    }
    static const char *KN[] = { "pm", "pr", "pb", "pw", "kk" };

    uint32_t from = w->tr_in_flushed;
    if (w->tr_in_head - from > WW_TR_IN_N) {
        from = w->tr_in_head - WW_TR_IN_N;
    }
    for (uint32_t i = from; i < w->tr_in_head; i++) {
        const struct ww_tr_in *e = &w->tr_in[i % WW_TR_IN_N];
        fprintf(f, "{\"type\":\"in\",\"k\":\"%s\",\"t\":%llu,\"d\":%llu,\"ts\":%.3f,\"x\":%.5f,\"y\":%.5f}\n",
                KN[e->kind & 7], (unsigned long long)e->t_recv,
                (unsigned long long)(e->t_done - e->t_recv), e->ts_client,
                (double)e->x, (double)e->y);
    }
    w->tr_in_flushed = w->tr_in_head;

    from = w->tr_frm_flushed;
    if (w->tr_frm_head - from > WW_TR_FR_N) {
        from = w->tr_frm_head - WW_TR_FR_N;
    }
    for (uint32_t i = from; i < w->tr_frm_head; i++) {
        const struct ww_tr_frm *e = &w->tr_frm[i % WW_TR_FR_N];
        fprintf(f, "{\"type\":\"frm\",\"id\":%u,\"t\":%llu,\"pts\":%lld,\"n\":%u,\"key\":%u,\"cx\":%.4f,\"cy\":%.4f}\n",
                e->id, (unsigned long long)e->t_send, (long long)e->pts, e->bytes, e->key,
                (double)e->cx, (double)e->cy);
    }
    w->tr_frm_flushed = w->tr_frm_head;
    fclose(f);
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
    {
        struct ww_tr_frm *fr = &w->tr_frm[w->tr_frm_head % WW_TR_FR_N];
        fr->t_send = now_us();
        fr->pts = (int64_t)nal->pts_us;
        fr->bytes = (uint32_t)nal->len;
        fr->id = w->frame_id;
        fr->key = nal->keyframe ? 1 : 0;
        fr->cx = w->cur_valid ? (float)w->cur_x : -1.0f;
        fr->cy = w->cur_valid ? (float)w->cur_y : -1.0f;
        w->tr_frm_head++;
        w->tr_dirty = true;
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
/* 把抓帧输出在布局中的逻辑位置/尺寸与整个布局尺寸同步给输入模块。
   zwlr_virtual_pointer 的 motion_absolute 是**整个布局**的归一化坐标，不是
   单输出坐标；多显示器时不做这层换算，光标会跑到别的屏（视频里看不到光标、
   点击位置也会错位）。

   布局信息用 hyprctl 读，而不是 wl_output 的 geometry/mode：
   Hyprland 给新建的 headless 输出发的 geometry 一直是 0,0，按它算会把
   第二块屏当成叠在同一位置，坐标仍然错。 */
static void update_input_layout(struct ww *w)
{
    if (!w->input) return;
    FILE *fp = popen("hyprctl monitors 2>/dev/null", "r");
    if (!fp) return;

    char line[512], name[64] = "";
    int mw = 0, mh = 0, mx = 0, my = 0;
    double sc = 1.0;
    bool in_mon = false;
    int ox = -1, oy = 0, ow = 0, oh = 0, lw = 0, lh = 0;

    while (fgets(line, sizeof line, fp)) {
        if (!strncmp(line, "Monitor ", 8)) {
            name[0] = 0; mw = mh = mx = my = 0; sc = 1.0;
            in_mon = sscanf(line + 8, "%63s", name) == 1;
            char *colon = strchr(name, ':');
            if (colon) *colon = 0;
        } else if (in_mon) {
            if (sscanf(line, " %dx%d@%*f at %dx%d", &mw, &mh, &mx, &my) == 4) {
                continue;
            }
            if (sscanf(line, " scale: %lf", &sc) != 1) continue;
            int lwid = (int)(mw / sc + 0.5);
            int lhei = (int)(mh / sc + 0.5);
            in_mon = false;
            if (lwid <= 0 || lhei <= 0) continue;   /* 禁用/未就绪的输出不计入布局 */
            if (mx + lwid > lw) lw = mx + lwid;
            if (my + lhei > lh) lh = my + lhei;
            if (w->output_name && !strcmp(name, w->output_name)) {
                ox = mx; oy = my; ow = lwid; oh = lhei;
            }
        }
    }
    pclose(fp);

    /* 找不到输出名（例如 hyprctl 不可用）时按单屏兜底，与旧行为一致 */
    if (ox < 0) { ox = 0; oy = 0; ow = (int)w->width; oh = (int)w->height; }
    if (lw <= 0) lw = ox + ow;
    if (lh <= 0) lh = oy + oh;
    if (ox != w->in_ox || oy != w->in_oy || ow != w->in_ow || oh != w->in_oh ||
        lw != w->in_lw || lh != w->in_lh) {
        fprintf(stderr, "[input] 布局 %d,%d %dx%d（总 %dx%d）\n", ox, oy, ow, oh, lw, lh);
        w->in_ox = ox; w->in_oy = oy; w->in_ow = ow; w->in_oh = oh;
        w->in_lw = lw; w->in_lh = lh;
    }
    ww_input_set_layout(w->input, ox, oy, ow, oh, lw, lh);
}

/* wl_output 的几何/模式/缩放只当“布局可能变了”的触发信号：
   Hyprland 下这几个值本身不可靠，真正的位置/尺寸以 hyprctl 为准。 */
static void ob_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                        int32_t subpixel, const char *make, const char *model, int32_t transform)
{
  struct out_probe *p = d;
  (void)o; (void)x; (void)y; (void)pw; (void)ph; (void)subpixel;
  (void)make; (void)model; (void)transform;
  update_input_layout(p->w);
}
static void ob_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t r)
{
  struct out_probe *p = d;
  (void)o; (void)flags; (void)w; (void)h; (void)r;
  update_input_layout(p->w);
}
static void ob_scale(void *d, struct wl_output *o, int32_t f)
{
  struct out_probe *p = d;
  (void)o; (void)f;
  update_input_layout(p->w);
}
static void ob_desc(void *d, struct wl_output *o, const char *s) { (void)d; (void)o; (void)s; }

static void ob_name(void *d, struct wl_output *o, const char *name)
{
    (void)o;
    struct out_probe *p = d;
    snprintf(p->name, sizeof(p->name), "%s", name ? name : "");
}

/* done 在 v4 里是最后一条事件，此时 name 已知，可以按 --output 匹配 */
static void ob_done(void *d, struct wl_output *o)
{
    (void)o;
    struct out_probe *p = d;
    struct ww *w = p->w;
    if (w->output) return;
    const char *want = w->output_name;
    if (want && *want) {
        if (!p->name[0] || strcmp(p->name, want) != 0) return;
    }
    w->output = p->o;
    update_input_layout(w);
}

static const struct wl_output_listener out_listener = {
    .geometry = ob_geometry,
    .mode = ob_mode,
    .done = ob_done,
    .scale = ob_scale,
    .name = ob_name,
    .description = ob_desc,
};

static void on_global(void *data, struct wl_registry *r, uint32_t name, const char *iface,
                      uint32_t ver)
{
    struct ww *w = data;
    if (!strcmp(iface, wl_output_interface.name)) {
        if (w->n_probes >= WW_MAX_OUT) return;
        struct out_probe *p = &w->probes[w->n_probes++];
        memset(p, 0, sizeof(*p));
        p->w = w;
        p->gname = name;
        p->o = wl_registry_bind(r, name, &wl_output_interface, ver >= 4 ? 4 : ver);
        wl_output_add_listener(p->o, &out_listener, p);
        w->layout_dirty = true;
        fprintf(stderr, "[cap] 发现输出 global=%u（共 %d 个）\n", name, w->n_probes);
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
static void on_global_remove(void *d, struct wl_registry *r, uint32_t n)
{
    struct ww *w = d;
    (void)r;
    for (int i = 0; i < w->n_probes; i++) {
        if (w->probes[i].gname != n) continue;
        /* 输出被移除后必须立刻缩回布局，否则输入坐标会按已经不存在的屏幕换算 */
        if (w->probes[i].o == w->output) w->output = NULL;
        wl_output_destroy(w->probes[i].o);
        memmove(&w->probes[i], &w->probes[i + 1],
                (size_t)(w->n_probes - i - 1) * sizeof(w->probes[0]));
        w->n_probes--;
        w->layout_dirty = true;
        break;
    }
}
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
        /* 创建失败不能逐帧重试：每帧都会泄漏一次 ffmpeg/DRM 资源，几十秒就耗尽 fd */
        if (!w->enc && now_us() < w->enc_retry_at) {
            return;
        }
        fprintf(stderr, "[cap] 帧尺寸 %ux%u，编码器 %ux%u → 重建\n",
                fw, fh, w->enc_w, w->enc_h);
        if (w->enc) {
            ww_enc_destroy(w->enc);
            w->enc = NULL;
        }
        w->enc = ww_enc_create_full(w->enc_kind, w->drm, (int)fw, (int)fh,
                                    w->fps, w->qp, w->chroma444, w->bitrate_bps);
        if (!w->enc) {
            if (encoder_fallback_420(w)) {
                w->enc_w = w->enc_h = 0;   /* 下一帧按 4:2:0 重建 */
                return;
            }
            fprintf(stderr, "[cap] 编码器重建失败（%ux%u），1 秒后重试\n", fw, fh);
            w->enc_w = w->enc_h = 0;
            w->enc_retry_at = now_us() + 1000000;
            return;
        }
        w->enc_retry_at = 0;
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
        /* IDR 请求不能丢：提交失败时保留标记，否则客户端要等下一个 GOP 才恢复 */
        w->need_idr = w->need_idr || idr;
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

/* ── 零拷贝帧回调：NV12 已由 CUDA 转换好，直接送编码器 ────────── */
#ifdef HAVE_SC_CAPTURE
static void on_sc_frame(void *ud, const uint8_t *y, uint32_t ys,
                        const uint8_t *uv, uint32_t uvs,
                        uint32_t fw, uint32_t fh, uint64_t pts, double conv_ms)
{
    struct ww *w = ud;

    if (fw != w->enc_w || fh != w->enc_h) {
        if (!w->enc && now_us() < w->enc_retry_at) {
            return;
        }
        fprintf(stderr, "[cap] 帧尺寸 %ux%u，编码器 %ux%u → 重建\n",
                fw, fh, w->enc_w, w->enc_h);
        if (w->enc) {
            ww_enc_destroy(w->enc);
            w->enc = NULL;
        }
        w->enc = ww_enc_create_full(w->enc_kind, w->drm, (int)fw, (int)fh,
                                    w->fps, w->qp, w->chroma444, w->bitrate_bps);
        if (!w->enc) {
            if (encoder_fallback_420(w)) {
                w->enc_w = w->enc_h = 0;   /* 下一帧按 4:2:0 重建 */
                return;
            }
            fprintf(stderr, "[cap] 编码器重建失败（%ux%u），1 秒后重试\n", fw, fh);
            w->enc_w = w->enc_h = 0;
            w->enc_retry_at = now_us() + 1000000;
            return;
        }
        w->enc_retry_at = 0;
        w->enc_w = fw;
        w->enc_h = fh;
        w->width = fw;
        w->height = fh;
        ww_input_set_geometry(w->input, (int)fw, (int)fh);
        w->need_idr = true;
    }

    w->capture_ts_us = pts;

    bool idr = w->need_idr;
    w->need_idr = false;

    if (ww_enc_submit_nv12(w->enc, y, (int)ys, uv, (int)uvs, fw, fh,
                           (int64_t)pts, idr) != 0) {
        w->need_idr = w->need_idr || idr;
        static bool logged;
        if (!logged) {
            logged = true;
            fprintf(stderr, "[cap] submit_nv12 失败（frame=%ux%u enc=%p）\n",
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
    w->acc_conv_us += (uint64_t)(conv_ms * 1000.0);
    w->acc_enc_us += t_end - t_conv;
    w->acc_n++;
    w->last_frame_us = t_end;
}
#endif

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
    const char *gbm_node = "/dev/dri/card1";
    bool gbm_set = false;
    const char *video_path = NULL;
    const char *ctrl_path = NULL;
    const char *audio_path = NULL;   /* --audio：不给就整条音频链路关闭 */
    int qp = 20, fps = 60;   /* CQP 是唯一可调项：20 明显比默认值清晰，码率仍可接受 */
    double bitrate_mbps = 0; /* --bitrate：0 = 关闭目标码率，走 QP（集显只支持这条） */
    bool dump_once = false;
    uint32_t max_w = 3200, max_h = 1800;   /* 默认上限：本机 4K 回读撑不住 30fps */
    const char *enc_kind = "vaapi";        /* 默认集显；--enc nvenc 走独显 */
    bool enc_set = false;
    const char *cap_mode = "shm";          /* --capture shm | dmabuf | sc（零拷贝，需 EGL/GL） */
    const char *output_name = "eDP-1";     /* --output：Hyprland 输出名 */

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--drm") && i + 1 < argc) drm = argv[++i];
        else if (!strcmp(argv[i], "--gbm") && i + 1 < argc) { gbm_node = argv[++i]; gbm_set = true; }
        else if (!strcmp(argv[i], "--video") && i + 1 < argc) video_path = argv[++i];
        else if (!strcmp(argv[i], "--ctrl") && i + 1 < argc) ctrl_path = argv[++i];
        else if (!strcmp(argv[i], "--audio") && i + 1 < argc) audio_path = argv[++i];
        else if (!strcmp(argv[i], "--qp") && i + 1 < argc) qp = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bitrate") && i + 1 < argc) bitrate_mbps = atof(argv[++i]);
        else if (!strcmp(argv[i], "--dump")) dump_once = true;
        else if (!strcmp(argv[i], "--capture") && i + 1 < argc) cap_mode = argv[++i];
        else if (!strcmp(argv[i], "--output") && i + 1 < argc) output_name = argv[++i];
        else if (!strcmp(argv[i], "--enc") && i + 1 < argc) { enc_kind = argv[++i]; enc_set = true; }
        else if (!strcmp(argv[i], "--max") && i + 1 < argc) {
            unsigned tw = 0, th = 0;
            if (sscanf(argv[++i], "%ux%u", &tw, &th) == 2) { max_w = tw; max_h = th; }
        }
    }
    bool cap_dmabuf = !strcmp(cap_mode, "dmabuf");
    bool cap_sc = !strcmp(cap_mode, "sc");
#ifndef HAVE_SC_CAPTURE
    if (cap_sc) {
        fprintf(stderr, "本版本未编译零拷贝抓帧后端（缺 EGL/GL）\n");
        return 2;
    }
#endif
#ifdef HAVE_SC_CAPTURE
    /* 零拷贝路径只走独显：默认 gbm 节点与编码器都跟着切到 NVIDIA */
    if (cap_sc && !enc_set) {
        enc_kind = "nvenc";
    }
    if (cap_sc && !gbm_set) {
        gbm_node = "/dev/dri/card0";
    }
#endif
    if (!video_path || !ctrl_path) {
        fprintf(stderr, "用法: wwcap --video <sock> --ctrl <sock> [--audio <sock>] "
                        "[--drm node] [--qp N] [--fps N] [--bitrate Mbps]\n");
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
    /* -1 保证第一次算出的布局一定会同步/打日志 */
    w.in_ox = w.in_oy = w.in_ow = w.in_oh = w.in_lw = w.in_lh = -1;
    w.audio_fd = -1;
    w.fps = fps;
    w.qp = qp;
    w.strategy = WW_STRAT_BALANCE;   /* 默认：平衡 */
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
    w.bitrate_bps = bitrate_mbps > 0 ? (int64_t)(bitrate_mbps * 1e6) : 0;
    /* 先探能力：不支持 CBR 的编码器上目标码率必须清零，否则 UI 会显示一个假设置值 */
    bitrate_refresh(&w);

    w.dpy = wl_display_connect(NULL);
    if (!w.dpy) { fprintf(stderr, "无法连接 Wayland\n"); return 1; }

    w.reg = wl_display_get_registry(w.dpy);
    wl_registry_add_listener(w.reg, &reg_listener, &w);
    wl_display_roundtrip(w.dpy);
    wl_display_roundtrip(w.dpy);   /* 第二次：等 wl_output 的 name / done */
    if (!w.output && w.n_probes > 0) {
        fprintf(stderr, "[cap] 未找到输出 %s，退回第一个 wl_output\n", output_name);
        w.output = w.probes[0].o;
    }
    if (!w.output) {
        fprintf(stderr, "缺少 wl_output\n");
        return 1;
    }

    /* 默认走 screencopy+shm：不需要 ext-capture / gbm。--capture dmabuf 才走扩展通路 */
    w.use_shm = !cap_dmabuf && !cap_sc;

    if (cap_dmabuf) {
        if (!w.source_mgr || !w.capture_mgr || !w.dmabuf) {
            fprintf(stderr, "缺少 ext-image-copy-capture 全局对象\n");
            return 1;
        }
        /* gbm（与 compositor 同卡） */
        const char *nodes[] = { gbm_node, "/dev/dri/card1", "/dev/dri/card0", NULL };
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

        w.enc = ww_enc_create_full(enc_kind, drm, (int)w.width, (int)w.height,
                                   fps, qp, w.chroma444, w.bitrate_bps);
        if (!w.enc && encoder_fallback_420(&w)) {
            w.enc = ww_enc_create_full(enc_kind, drm, (int)w.width, (int)w.height,
                                       fps, qp, false, w.bitrate_bps);
        }
        if (!w.enc) { fprintf(stderr, "编码器初始化失败\n"); return 1; }
        w.enc_w = w.width;
        w.enc_h = w.height;
    }

    w.input = ww_input_create(w.dpy, w.seat);
    if (w.input) {
        ww_input_attach(w.input, w.kbmgr, w.ptrmgr, w.seat);
        if (w.width && w.height) {
            ww_input_set_geometry(w.input, (int)w.width, (int)w.height);
        }
        update_input_layout(&w);
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

    if (w.use_shm) {
        w.shm = ww_shm_create(w.dpy, w.output, fps, on_shm_frame, &w);
        if (!w.shm) {
            fprintf(stderr, "[cap] screencopy 不可用\n");
            return 1;
        }
    }

#ifdef HAVE_SC_CAPTURE
    if (cap_sc) {
        w.sc = ww_sc_create(w.dpy, w.output, gbm_node, fps, on_sc_frame, &w);
        if (!w.sc) {
            fprintf(stderr, "[cap] 零拷贝抓帧初始化失败\n");
            return 1;
        }
    }
#endif

    /* 探测 4:4:4 能力并写出能力文件（网关 /capabilities 读取） */
    chroma_refresh(&w);

    /* IPC */
    w.video_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    /* 默认 SO_SNDBUF 只有 ~200KB，4K 关键帧瞬间就能塞满而整帧丢分片 */
    int sndbuf = 4 * 1024 * 1024;
    setsockopt(w.video_fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    mkaddr(&w.video_addr, video_path);

    w.ctrl_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    struct sockaddr_un ctrl_addr;
    mkaddr(&ctrl_addr, ctrl_path);
    /* 单实例锁：仅靠 unlink+bind 拦不住第二个进程（它会把旧 socket 文件删掉再绑新路径），
       结果 Hyprland 里多出一个虚拟指针、旧进程收不到输入。 */
    char lock_path[512];
    snprintf(lock_path, sizeof(lock_path), "%s.lock", ctrl_path);
    int lock_fd = open(lock_path, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
    if (lock_fd < 0 || flock(lock_fd, LOCK_EX | LOCK_NB) < 0) {
        fprintf(stderr, "[cap] 已有 wwcap 在运行（%s 被占用）\n", lock_path);
        return 1;
    }
    unlink(ctrl_path);
    if (bind(w.ctrl_fd, (struct sockaddr *)&ctrl_addr, sizeof(ctrl_addr)) < 0) {
        fprintf(stderr, "[cap] 绑定 ctrl socket 失败: %s（可能已有 wwcap 在运行）\n",
                strerror(errno));
        return 1;
    }

    /* 音频：只发不收（上行是二期），因此不需要 bind，也没有额外 poll fd */
    if (audio_path) {
        w.audio_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (w.audio_fd >= 0) {
            int abuf = 1024 * 1024;   /* 20ms 一包、每包几百字节，1MB 足够吸收瞬时抖动 */
            setsockopt(w.audio_fd, SOL_SOCKET, SO_SNDBUF, &abuf, sizeof(abuf));
            w.aud = ww_audio_create(w.audio_fd, audio_path, WW_AUDIO_BITRATE);
            if (!w.aud) {
                fprintf(stderr, "[aud] 初始化失败，串流将没有声音（视频不受影响）\n");
                close(w.audio_fd);
                w.audio_fd = -1;
            }
        } else {
            fprintf(stderr, "[aud] 创建音频 socket 失败: %s\n", strerror(errno));
        }
    }

    if (cap_dmabuf && submit_frame(&w) != 0) { fprintf(stderr, "首帧提交失败\n"); return 1; }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    w.last_frame_us = now_us();
    /* 启动时默认无会话：30 秒内没人连就休眠，别空转编码器 */
    w.idle_since_us = w.last_frame_us;
    fprintf(stderr, "[cap] 运行中\n");

    struct pollfd pfd[2];
    pfd[0].fd = wl_display_get_fd(w.dpy); pfd[0].events = POLLIN;
    pfd[1].fd = w.ctrl_fd; pfd[1].events = POLLIN;

    uint64_t stat_t = now_us();
    uint64_t stat_frames = 0;
    uint64_t aud_pkts_prev = 0, aud_bytes_prev = 0;

    for (;;) {
        if (g_stop) break;
        wl_display_flush(w.dpy);
        /* 轮询粒度要远小于帧间隔，否则抓帧节拍会被 poll 超时量化掉；
           休眠时放宽到 200ms，避免空转唤醒 CPU */
        if (poll(pfd, 2, w.suspended ? 200 : 1) < 0 && errno != EINTR) break;

        /* compositor 退出/fd 失效时不处理会变成 1ms 忙循环，直接把 CPU 打满 */
        if (pfd[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "[cap] Wayland 连接异常退出\n");
            break;
        }
        if (pfd[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            fprintf(stderr, "[cap] 控制 socket 异常退出\n");
            break;
        }

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

        /* 追踪落盘：5 秒一次整体覆盖写，只有产生新事件时才写 */
        {
            uint64_t tn = now_us();
            if (w.tr_dirty && tn >= w.tr_flush_at) {
                trace_flush(&w);
                w.tr_flush_at = tn + 5000000ull;
            }
        }

        /* 新输出刚绑定：roundtrip 一次拿到它的初始 geometry/mode/scale，
           再重算输入布局（否则新插入的显示器不会计入布局） */
        if (w.layout_dirty) {
            w.layout_dirty = false;
            wl_display_roundtrip(w.dpy);
            update_input_layout(&w);
        }

        uint64_t t = now_us();

        /* 没有会话满 30 秒：停抓帧并释放编码器（GPU 编码会话归零），
           进程和 Wayland 连接保持常驻，下一个会话接入时立即恢复 */
        if (!w.suspended && !w.active && w.idle_since_us &&
            t - w.idle_since_us > 30ull * 1000000ull) {
            w.suspended = true;
            if (w.enc) {
                ww_enc_destroy(w.enc);
                w.enc = NULL;
            }
            w.enc_w = w.enc_h = 0;
            fprintf(stderr, "[cap] 无会话 30 秒，进入休眠（已停抓帧/编码）\n");
        }
        if (w.suspended && w.active) {
            w.suspended = false;
            w.need_idr = true;   /* 唤醒后第一帧就是 IDR，客户端不必等下一个 GOP */
            stat_t = t;
            stat_frames = w.frames_sent;
            w.acc_cap_us = w.acc_conv_us = w.acc_enc_us = w.acc_n = 0;
            fprintf(stderr, "[cap] 会话接入，恢复采集\n");
        }

        /* shm 模式按固定帧率主动抓帧；ext 模式靠 damage + 空闲强制重绘 */
        if (w.suspended) {
            /* 休眠中不发起任何抓帧请求 */
        } else if (w.use_shm) {
            /* 只在真正发起抓帧时打点，否则 cap 统计会被每轮循环重置 */
            if (!ww_shm_pending(w.shm)) {
                w.t_tick_us = now_us();
            }
            ww_shm_tick(w.shm);
#ifdef HAVE_SC_CAPTURE
        } else if (w.sc) {
            ww_sc_tick(w.sc);
#endif
        } else if (w.last_frame_us && t - w.last_frame_us > 800000) {
            system("hyprctl dispatch forcerendererreload >/dev/null 2>&1");
            w.last_frame_us = t;
        }

        if (!w.suspended && t - stat_t >= 1000000) {
            double n = w.acc_n ? (double)w.acc_n : 1.0;
            double ec = 0, eu = 0, es = 0;
            if (w.enc) {
                ww_enc_take_stats(w.enc, &ec, &eu, &es);
            }
            fprintf(stderr, "[enc] 转换 %.1fms 上传 %.1fms 提交 %.1fms\n", ec, eu, es);

            double fps_now = (double)(w.frames_sent - stat_frames) * 1e6 / (double)(t - stat_t);

            /* 流畅优先：不动分辨率，只在 QP 24~30 之间按实测帧率浮动 */
            if (w.strategy == WW_STRAT_SMOOTH && w.enc) {
                double target = (double)(w.fps > 0 ? w.fps : 60);
                int nq = w.qp;
                if (fps_now < target * 0.85) {
                    nq = w.qp + 2;
                } else if (fps_now > target * 0.95) {
                    nq = w.qp - 1;
                }
                if (nq < 24) nq = 24;
                if (nq > 30) nq = 30;
                if (nq != w.qp) {
                    w.qp = nq;
                    ww_enc_set_qp(w.enc, nq);
                    w.need_idr = true;
                    fprintf(stderr, "[cap] 流畅优先：QP -> %d（实测 %.0f / 目标 %.0f fps）\n",
                            nq, fps_now, target);
                }
            }

            /* 清晰优先的自适应：按用户选择的目标帧率判断余量，
               以前写死 26/46，选了 30fps 也会被误判、4K 降档后永远升不回去 */
            if (w.req_w && w.auto_adapt) {
                double target = (double)(w.fps > 0 ? w.fps : 60);
                if (fps_now < target * 0.62 && w.cur_preset > 0) {
                    if (!w.slow_since) {
                        w.slow_since = t;
                    } else if (t - w.slow_since > 3000000) {
                        fprintf(stderr, "[cap] 帧率 %.0f 不足（目标 %.0f），降档\n", fps_now, target);
                        apply_preset(&w, w.cur_preset - 1);
                        w.slow_since = 0;
                    }
                    w.fast_since = 0;
                } else if (fps_now > target * 0.92 && w.cur_preset < w.max_preset) {
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
            double cap_ms = w.shm ? ww_shm_take_cap_ms(w.shm) : 0.0;
#ifdef HAVE_SC_CAPTURE
            if (!w.shm && w.sc) {
                cap_ms = ww_sc_take_cap_ms(w.sc);
            }
#endif
            fprintf(stderr, "[cap] %.1f fps, %.2f Mbps%s, frames=%llu | cap %.1fms conv %.1fms enc %.1fms\n",
                    (double)(w.frames_sent - stat_frames) * 1e6 / (double)(t - stat_t),
                    (double)w.bytes_sent * 8.0 / (double)(t - stat_t),
                    w.bitrate_bps > 0 ? " (目标码率)" : " (CQP)",
                    (unsigned long long)w.frames_sent,
                    cap_ms,
                    (double)w.acc_conv_us / n / 1000.0,
                    (double)w.acc_enc_us / n / 1000.0);
            if (w.aud) {
                uint64_t ap = 0, ab = 0, ad = 0;
                ww_audio_stats(w.aud, &ap, &ab, &ad);
                fprintf(stderr, "[aud] %.1f pkt/s, %.0f kbps, 队列丢弃=%llu\n",
                        (double)(ap - aud_pkts_prev) * 1e6 / (double)(t - stat_t),
                        (double)(ab - aud_bytes_prev) * 8.0 * 1e6 / (double)(t - stat_t) / 1000.0,
                        (unsigned long long)ad);
                aud_pkts_prev = ap;
                aud_bytes_prev = ab;
            }
            w.acc_cap_us = w.acc_conv_us = w.acc_enc_us = w.acc_n = 0;
            stat_t = t;
            stat_frames = w.frames_sent;
            w.bytes_sent = 0;
        }
    }

    ww_enc_destroy(w.enc);
#ifdef HAVE_SC_CAPTURE
    ww_sc_destroy(w.sc);
#endif
    ww_shm_destroy(w.shm);
    ww_input_destroy(w.input);
    /* 先停采集线程再关 fd；退出时把目标机扬声器恢复原状态 */
    if (w.aud) ww_audio_destroy(w.aud);
    if (w.audio_fd >= 0) close(w.audio_fd);
    sink_mute_restore();
    if (w.video_fd >= 0) close(w.video_fd);
    if (w.ctrl_fd >= 0) close(w.ctrl_fd);
    unlink(ctrl_path);
    wl_display_disconnect(w.dpy);
    return 0;
}
