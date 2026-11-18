/*
 * wlr-screencopy + wl_shm 抓帧（双缓冲流水线）
 *
 * 为什么用它：ext-image-copy-capture 的 dmabuf 拷贝在本机拿不到有效像素（读出来全黑），
 * 而 grim（走 screencopy/shm）能正确取到画面。
 *
 * 关键优化：4K 下一次 GPU→CPU 回读约 25ms，与「色彩转换+编码」是串行的。
 * 用两个 shm 槽位交替：ready 后立刻发起下一帧抓取，再去处理当前帧，
 * 让回读与转换重叠，帧率从 ~26 提升到 ~40。
 */

#define _GNU_SOURCE

#include "capture_shm.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "generated/wlr-screencopy-unstable-v1.h"

#define WW_SLOTS 2

struct shm_slot {
    struct wl_buffer *buf;
    void *data;
    size_t size;
    uint32_t w, h, stride;
};

struct ww_shm {
    struct wl_display *dpy;
    struct wl_output *output;

    struct zwlr_screencopy_manager_v1 *mgr;
    struct wl_shm *shm;

    struct shm_slot slots[WW_SLOTS];
    int cur;              /* 当前正在填充/刚填好的槽位 */

    int fps;
    uint64_t last_us;
    bool pending;
    bool ready;
    /* 抓帧往返统计：发起 request → ready */
    uint64_t req_us, sum_cap_us, n_cap;

    ww_frame_cb cb;
    void *ud;
};

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* ── 第二套 registry：只取 screencopy 与 shm ─────────────────── */
static void reg_global(void *data, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
    struct ww_shm *s = data;
    if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name)) {
        s->mgr = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, ver >= 3 ? 3 : ver);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        s->shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_listener = { .global = reg_global, .global_remove = reg_remove };

/* ── shm 槽位 ───────────────────────────────────────────────── */
static int ensure_slot(struct ww_shm *s, int idx, uint32_t format, uint32_t w, uint32_t h, uint32_t stride)
{
    struct shm_slot *sl = &s->slots[idx];
    if (sl->buf && sl->w == w && sl->h == h && sl->stride == stride) {
        return 0;
    }
    if (sl->buf) {
        wl_buffer_destroy(sl->buf);
        munmap(sl->data, sl->size);
        sl->buf = NULL;
    }

    size_t size = (size_t)stride * h;
    int fd = memfd_create("ww-shm", MFD_CLOEXEC);
    if (fd < 0) {
        return -1;
    }
    if (ftruncate(fd, (off_t)size) < 0) {
        close(fd);
        return -1;
    }
    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return -1;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(s->shm, fd, (int32_t)size);
    struct wl_buffer *buf = wl_shm_pool_create_buffer(pool, 0, (int32_t)w, (int32_t)h,
                                                      (int32_t)stride, format);
    wl_shm_pool_destroy(pool);
    close(fd);
    if (!buf) {
        munmap(data, size);
        return -1;
    }
    sl->buf = buf;
    sl->data = data;
    sl->size = size;
    sl->w = w;
    sl->h = h;
    sl->stride = stride;
    fprintf(stderr, "[shm] 槽位%d 就绪 %ux%u stride=%u\n", idx, w, h, stride);
    return 0;
}

/* ── frame 事件 ─────────────────────────────────────────────── */
static void fr_buffer(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t format,
                      uint32_t w, uint32_t h, uint32_t stride)
{
    struct ww_shm *s = data;
    (void)f;
    if (ensure_slot(s, s->cur, format, w, h, stride) == 0) {
        s->ready = true;
    }
}

static void fr_flags(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t flags)
{ (void)data; (void)f; (void)flags; }

static void fr_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *f)
{
    struct ww_shm *s = data;
    struct shm_slot *sl = &s->slots[s->cur];
    if (!sl->buf) {
        s->pending = false;
        zwlr_screencopy_frame_v1_destroy(f);
        return;
    }
    zwlr_screencopy_frame_v1_copy(f, sl->buf);
}

static void fr_ready(void *data, struct zwlr_screencopy_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c)
{
    (void)a; (void)b; (void)c;
    struct ww_shm *s = data;
    zwlr_screencopy_frame_v1_destroy(f);

    if (s->req_us) {
        uint64_t now = now_us();
        if (now > s->req_us) {
            s->sum_cap_us += now - s->req_us;
        }
        s->n_cap++;
        s->req_us = 0;
    }

    int idx = s->cur;
    s->pending = false;
    s->cur = 1 - idx;                 /* 下一帧写另一个槽位，避免覆盖正在处理的帧 */
    ww_shm_tick(s);                   /* 先把下一帧抓起来（与下面的处理重叠） */

    struct shm_slot *sl = &s->slots[idx];
    if (s->cb && sl->buf) {
        s->cb(s->ud, sl->data, sl->stride, sl->w, sl->h, now_us());
    }
}

static void fr_failed(void *data, struct zwlr_screencopy_frame_v1 *f)
{
    struct ww_shm *s = data;
    zwlr_screencopy_frame_v1_destroy(f);
    s->pending = false;
    static int logged;
    if (!logged) {
        fprintf(stderr, "[shm] capture failed（后续静默）\n");
        logged = 1;
    }
}

static void fr_damage(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ (void)d; (void)f; (void)x; (void)y; (void)w; (void)h; }
static void fr_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h)
{
    (void)d; (void)f;
    static int logged;
    if (!logged) {
        fprintf(stderr, "[shm] compositor 提供 screencopy dmabuf fmt=0x%x %ux%u\n", fmt, w, h);
        logged = 1;
    }
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = fr_buffer,
    .flags = fr_flags,
    .ready = fr_ready,
    .failed = fr_failed,
    .damage = fr_damage,
    .linux_dmabuf = fr_dmabuf,
    .buffer_done = fr_buffer_done,
};

struct ww_shm *ww_shm_create(struct wl_display *dpy, struct wl_output *output,
                             int fps, ww_frame_cb cb, void *ud)
{
    struct ww_shm *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->dpy = dpy;
    s->output = output;
    s->fps = fps > 0 ? fps : 60;
    s->cb = cb;
    s->ud = ud;

    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, s);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);

    if (!s->mgr || !s->shm) {
        fprintf(stderr, "[shm] 缺少 screencopy/shm 全局\n");
        free(s);
        return NULL;
    }
    fprintf(stderr, "[shm] screencopy 就绪（双缓冲），目标 %d fps\n", s->fps);
    return s;
}

bool ww_shm_pending(struct ww_shm *s) { return s && s->pending; }

void ww_shm_tick(struct ww_shm *s)
{
    if (!s || !s->mgr || !s->output || s->pending) {
        return;
    }
    uint64_t interval = 1000000ull / (uint64_t)s->fps;
    if (s->last_us && now_us() - s->last_us < interval) {
        return;
    }
    s->last_us = now_us();
    s->req_us = s->last_us;
    s->pending = true;

    struct zwlr_screencopy_frame_v1 *f =
        zwlr_screencopy_manager_v1_capture_output(s->mgr, 0, s->output);
    if (!f) {
        s->pending = false;
        return;
    }
    zwlr_screencopy_frame_v1_add_listener(f, &frame_listener, s);
    wl_display_flush(s->dpy);
}

bool ww_shm_ready(struct ww_shm *s) { return s && s->ready; }

/* 取走抓帧往返平均耗时（毫秒），调用后清零 */
double ww_shm_take_cap_ms(struct ww_shm *s)
{
    if (!s || s->n_cap == 0) {
        return 0.0;
    }
    double ms = (double)s->sum_cap_us / (double)s->n_cap / 1000.0;
    s->sum_cap_us = 0;
    s->n_cap = 0;
    return ms;
}

void ww_shm_set_fps(struct ww_shm *s, int fps)
{
    if (!s) {
        return;
    }
    if (fps < 5) fps = 5;
    if (fps > 120) fps = 120;
    s->fps = fps;
    s->last_us = 0;   /* 立即生效，不等旧节拍 */
}

void ww_shm_destroy(struct ww_shm *s)
{
    if (!s) {
        return;
    }
    for (int i = 0; i < WW_SLOTS; i++) {
        if (s->slots[i].buf) {
            wl_buffer_destroy(s->slots[i].buf);
        }
        if (s->slots[i].data) {
            munmap(s->slots[i].data, s->slots[i].size);
        }
    }
    free(s);
}
