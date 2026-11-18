/*
 * wwcap 抓帧探针 —— 验证 ext-image-copy-capture + linux-dmabuf 零拷贝通路。
 *
 * 行为：
 *   1. 连接 Hyprland，绑定 ext_output_image_capture_source_manager_v1 /
 *      ext_image_copy_capture_manager_v1 / zwp_linux_dmabuf_v1
 *   2. 以第一个 wl_output 为源建立 capture session
 *   3. 用 gbm 分配 dmabuf buffer（优先 linear modifier），包成 wl_buffer 后提交抓帧
 *   4. 每帧记录 capture() -> ready 的耗时，可选把首帧落地为 PPM 做肉眼校验
 *
 * 用法: wwcap-probe [--frames N] [--dump out.ppm]
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

#include <gbm.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <wayland-client.h>

#include "generated/ext-image-capture-source-v1.h"
#include "generated/ext-image-copy-capture-v1.h"
#include "generated/linux-dmabuf-v1.h"

#ifndef DRM_FORMAT_ARGB8888
#include <drm_fourcc.h>
#endif

#define MAX_FORMATS 64

struct format_candidate {
    uint32_t fourcc;
    uint64_t modifier;
};

struct state {
    struct wl_display *display;
    struct wl_registry *registry;

    struct wl_output *output;
    struct ext_output_image_capture_source_manager_v1 *source_mgr;
    struct ext_image_copy_capture_manager_v1 *capture_mgr;
    struct zwp_linux_dmabuf_v1 *dmabuf;

    struct ext_image_capture_source_v1 *source;
    struct ext_image_copy_capture_session_v1 *session;

    /* session 协商结果 */
    uint32_t width, height;
    bool size_known;
    bool session_ready;
    struct format_candidate formats[MAX_FORMATS];
    int n_formats;

    /* gbm 分配器 */
    struct gbm_device *gbm;
    struct gbm_bo *bo;
    struct wl_buffer *buffer;
    bool buffer_busy;
    int dmabuf_fd;
    uint32_t stride;

    /* 帧统计 */
    int frames_done;
    int frames_target;
    int frames_failed;
    double latency_sum_ms;
    double latency_min_ms;
    double latency_max_ms;
    uint64_t capture_ts_ns;

    const char *dump_path;
    bool dumped;

    bool running;
};

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ── wl_buffer 释放 ───────────────────────────────────────────── */
static void buffer_release(void *data, struct wl_buffer *buffer)
{
    (void)buffer;
    struct state *s = data;
    s->buffer_busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

/* ── session 事件 ─────────────────────────────────────────────── */
static void session_buffer_size(void *data, struct ext_image_copy_capture_session_v1 *session,
                                uint32_t width, uint32_t height)
{
    (void)session;
    struct state *s = data;
    s->width = width;
    s->height = height;
    s->size_known = true;
    fprintf(stderr, "[session] buffer_size = %ux%u\n", width, height);
}

static void session_shm_format(void *data, struct ext_image_copy_capture_session_v1 *session,
                               uint32_t format)
{
    (void)data; (void)session; (void)format;
}

static void session_dmabuf_device(void *data, struct ext_image_copy_capture_session_v1 *session,
                                  struct wl_array *device)
{
    (void)session;
    struct state *s = data;
    if (device->size >= sizeof(dev_t)) {
        dev_t dev;
        memcpy(&dev, device->data, sizeof(dev_t));
        fprintf(stderr, "[session] dmabuf_device = %u:%u (大小 %zu 字节)\n",
                major(dev), minor(dev), device->size);
        if (s->gbm) {
            dev_t ours = gbm_device_get_fd(s->gbm); /* 仅记录，不比对 fd 与 dev */
            (void)ours;
        }
    }
}

static void session_dmabuf_format(void *data, struct ext_image_copy_capture_session_v1 *session,
                                 uint32_t format, struct wl_array *modifiers)
{
    (void)session;
    struct state *s = data;
    const uint64_t *mods = modifiers->data;
    size_t n = modifiers->size / sizeof(uint64_t);

    if (n == 0) {
        if (s->n_formats < MAX_FORMATS) {
            s->formats[s->n_formats].fourcc = format;
            s->formats[s->n_formats].modifier = 0;
            s->n_formats++;
        }
        return;
    }
    for (size_t i = 0; i < n && s->n_formats < MAX_FORMATS; i++) {
        s->formats[s->n_formats].fourcc = format;
        s->formats[s->n_formats].modifier = mods[i];
        s->n_formats++;
    }
}

static void session_done(void *data, struct ext_image_copy_capture_session_v1 *session)
{
    (void)session;
    struct state *s = data;
    s->session_ready = true;
}

static void session_stopped(void *data, struct ext_image_copy_capture_session_v1 *session)
{
    (void)session;
    struct state *s = data;
    fprintf(stderr, "[session] stopped —— 源已失效\n");
    s->running = false;
}

static const struct ext_image_copy_capture_session_v1_listener session_listener = {
    .buffer_size = session_buffer_size,
    .shm_format = session_shm_format,
    .dmabuf_device = session_dmabuf_device,
    .dmabuf_format = session_dmabuf_format,
    .done = session_done,
    .stopped = session_stopped,
};

/* ── PPM 落地（仅用于肉眼校验，不是生产路径）───────────────────── */
static int dump_ppm(struct state *s, const char *path)
{
    if (!s->bo) {
        return -1;
    }
    uint32_t stride = 0;
    void *map = NULL;
    map = gbm_bo_map(s->bo, 0, 0, s->width, s->height, GBM_BO_TRANSFER_READ, &stride, &map);
    if (!map) {
        fprintf(stderr, "[dump] gbm_bo_map 失败: %s\n", strerror(errno));
        return -1;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        gbm_bo_unmap(s->bo, map);
        return -1;
    }
    fprintf(f, "P6\n%u %u\n255\n", s->width, s->height);

    const uint8_t *row = map;
    uint8_t *line = malloc((size_t)s->width * 3);
    if (!line) {
        fclose(f);
        gbm_bo_unmap(s->bo, map);
        return -1;
    }

    for (uint32_t y = 0; y < s->height; y++) {
        const uint8_t *src = row + (size_t)y * stride;
        for (uint32_t x = 0; x < s->width; x++) {
            /* ARGB8888 小端内存布局为 B G R A */
            line[x * 3 + 0] = src[x * 4 + 2];
            line[x * 3 + 1] = src[x * 4 + 1];
            line[x * 3 + 2] = src[x * 4 + 0];
        }
        fwrite(line, 1, (size_t)s->width * 3, f);
    }
    free(line);
    fclose(f);
    /* 注意：i915 + mesa 26 上 gbm_bo_unmap 会在 libgallium 内部崩溃。
       且生产路径不会把帧回读到 CPU —— 这里仅作一次性肉眼校验，故不 unmap。 */
    fprintf(stderr, "[dump] 已写入 %s (%ux%u)\n", path, s->width, s->height);
    return 0;
}

/* ── 提交一帧 ─────────────────────────────────────────────────── */
static struct ext_image_copy_capture_frame_v1 *g_frame;

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame);
static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t reason);

static void frame_transform(void *data, struct ext_image_copy_capture_frame_v1 *frame,
                            uint32_t transform)
{
    (void)data; (void)frame; (void)transform;
}

/* 损坏区域：首帧是整屏，后续帧只报增量。记录面积用于后续做 damage 驱动 */
static void frame_damage(void *data, struct ext_image_copy_capture_frame_v1 *frame,
                         int32_t x, int32_t y, int32_t width, int32_t height)
{
    (void)data; (void)frame;
    (void)x; (void)y;
    (void)width; (void)height;
}

static void frame_presentation_time(void *data, struct ext_image_copy_capture_frame_v1 *frame,
                                   uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec)
{
    (void)data; (void)frame;
    (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;
}

static const struct ext_image_copy_capture_frame_v1_listener frame_listener = {
    .transform = frame_transform,
    .damage = frame_damage,
    .presentation_time = frame_presentation_time,
    .ready = frame_ready,
    .failed = frame_failed,
};

static int submit_frame(struct state *s);

static void frame_ready(void *data, struct ext_image_copy_capture_frame_v1 *frame)
{
    struct state *s = data;
    uint64_t elapsed = now_ns() - s->capture_ts_ns;
    double ms = (double)elapsed / 1e6;

    s->frames_done++;
    s->latency_sum_ms += ms;
    if (s->frames_done == 1 || ms < s->latency_min_ms) s->latency_min_ms = ms;
    if (ms > s->latency_max_ms) s->latency_max_ms = ms;

    if (s->dump_path && !s->dumped) {
        if (dump_ppm(s, s->dump_path) == 0) {
            s->dumped = true;
        }
    }

    ext_image_copy_capture_frame_v1_destroy(frame);
    g_frame = NULL;
    s->buffer_busy = false;

    if (s->frames_done >= s->frames_target) {
        s->running = false;
        return;
    }
    if (submit_frame(s) != 0) {
        s->running = false;
    }
}

static void frame_failed(void *data, struct ext_image_copy_capture_frame_v1 *frame, uint32_t reason)
{
    struct state *s = data;
    const char *reason_str =
        reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_UNKNOWN ? "unknown" :
        reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_BUFFER_CONSTRAINTS ? "buffer_constraints" :
        reason == EXT_IMAGE_COPY_CAPTURE_FRAME_V1_FAILURE_REASON_STOPPED ? "stopped" : "?";
    fprintf(stderr, "[frame] failed: %s (%u)\n", reason_str, reason);
    s->frames_failed++;
    ext_image_copy_capture_frame_v1_destroy(frame);
    g_frame = NULL;
    s->buffer_busy = false;
    s->running = false;
}

static int submit_frame(struct state *s)
{
    if (!g_frame) {
        g_frame = ext_image_copy_capture_session_v1_create_frame(s->session);
        if (!g_frame) {
            return -1;
        }
        ext_image_copy_capture_frame_v1_add_listener(g_frame, &frame_listener, s);
    }

    ext_image_copy_capture_frame_v1_attach_buffer(g_frame, s->buffer);
    ext_image_copy_capture_frame_v1_damage_buffer(g_frame, 0, 0, s->width, s->height);

    s->capture_ts_ns = now_ns();
    ext_image_copy_capture_frame_v1_capture(g_frame);
    return 0;
}

/* ── registry ─────────────────────────────────────────────────── */
static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    struct state *s = data;

    if (strcmp(interface, wl_output_interface.name) == 0) {
        if (!s->output) {
            s->output = wl_registry_bind(registry, name, &wl_output_interface, version >= 4 ? 4 : version);
            fprintf(stderr, "[registry] wl_output 已绑定（name=%u, v%u）\n", name, version);
        }
    } else if (strcmp(interface, ext_output_image_capture_source_manager_v1_interface.name) == 0) {
        s->source_mgr = wl_registry_bind(registry, name,
                                         &ext_output_image_capture_source_manager_v1_interface, 1);
        fprintf(stderr, "[registry] ext_output_image_capture_source_manager_v1\n");
    } else if (strcmp(interface, ext_image_copy_capture_manager_v1_interface.name) == 0) {
        s->capture_mgr = wl_registry_bind(registry, name,
                                          &ext_image_copy_capture_manager_v1_interface, 1);
        fprintf(stderr, "[registry] ext_image_copy_capture_manager_v1\n");
    } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
        s->dmabuf = wl_registry_bind(registry, name, &zwp_linux_dmabuf_v1_interface,
                                     version >= 3 ? 3 : version);
        fprintf(stderr, "[registry] zwp_linux_dmabuf_v1 (v%u)\n", version);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

/* ── dmabuf wl_buffer 构造 ───────────────────────────────────── */
static struct wl_buffer *create_dmabuf_buffer(struct state *s, int fd, uint32_t fourcc,
                                              uint32_t w, uint32_t h, uint32_t stride,
                                              uint64_t modifier)
{
    struct zwp_linux_buffer_params_v1 *params = zwp_linux_dmabuf_v1_create_params(s->dmabuf);
    if (!params) {
        return NULL;
    }
    zwp_linux_buffer_params_v1_add(params, fd, 0, 0, stride,
                                   (uint32_t)(modifier >> 32), (uint32_t)(modifier & 0xffffffffu));
    struct wl_buffer *buf = zwp_linux_buffer_params_v1_create_immed(params, w, h, fourcc, 0);
    zwp_linux_buffer_params_v1_destroy(params);
    return buf;
}

static const char *fourcc_name(uint32_t f)
{
    switch (f) {
    case DRM_FORMAT_ARGB8888: return "ARGB8888";
    case DRM_FORMAT_XRGB8888: return "XRGB8888";
    case DRM_FORMAT_ABGR8888: return "ABGR8888";
    case DRM_FORMAT_XBGR8888: return "XBGR8888";
    default: return "?";
    }
}

int main(int argc, char **argv)
{
    struct state s = {
        .frames_target = 120,
        .latency_min_ms = 1e9,
        .running = true,
    };

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            s.frames_target = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
            s.dump_path = argv[++i];
        } else {
            fprintf(stderr, "用法: %s [--frames N] [--dump out.ppm]\n", argv[0]);
            return 2;
        }
    }

    s.display = wl_display_connect(NULL);
    if (!s.display) {
        fprintf(stderr, "无法连接 Wayland（检查 WAYLAND_DISPLAY）\n");
        return 1;
    }
    fprintf(stderr, "[init] Wayland 已连接\n");

    s.registry = wl_display_get_registry(s.display);
    wl_registry_add_listener(s.registry, &registry_listener, &s);
    wl_display_roundtrip(s.display);

    if (!s.source_mgr || !s.capture_mgr || !s.dmabuf || !s.output) {
        fprintf(stderr, "缺少必要全局：source_mgr=%p capture_mgr=%p dmabuf=%p output=%p\n",
                (void *)s.source_mgr, (void *)s.capture_mgr, (void *)s.dmabuf, (void *)s.output);
        return 1;
    }

    /* gbm 设备：与 compositor 同卡（iGPU card1） */
    const char *nodes[] = {"/dev/dri/card1", "/dev/dri/renderD129", "/dev/dri/card0", NULL};
    for (int i = 0; nodes[i] && !s.gbm; i++) {
        int fd = open(nodes[i], O_RDWR | O_CLOEXEC);
        if (fd < 0) continue;
        s.gbm = gbm_create_device(fd);
        if (s.gbm) {
            fprintf(stderr, "[init] gbm 设备: %s (%s)\n", nodes[i], gbm_device_get_backend_name(s.gbm));
        } else {
            close(fd);
        }
    }
    if (!s.gbm) {
        fprintf(stderr, "无法创建 gbm 设备\n");
        return 1;
    }

    /* 建立 capture session */
    s.source = ext_output_image_capture_source_manager_v1_create_source(s.source_mgr, s.output);
    s.session = ext_image_copy_capture_manager_v1_create_session(s.capture_mgr, s.source, 0);
    ext_image_copy_capture_session_v1_add_listener(s.session, &session_listener, &s);

    while (!s.session_ready && wl_display_dispatch(s.display) != -1) {}
    if (!s.size_known) {
        fprintf(stderr, "session 未给出 buffer_size\n");
        return 1;
    }

    /* 选择格式：优先 linear modifier 的 ARGB/XRGB */
    uint32_t fourcc = 0;
    uint64_t modifier = 0;
    bool found = false;
    for (int pass = 0; pass < 2 && !found; pass++) {
        for (int i = 0; i < s.n_formats; i++) {
            bool is_rgb = s.formats[i].fourcc == DRM_FORMAT_ARGB8888 ||
                          s.formats[i].fourcc == DRM_FORMAT_XRGB8888 ||
                          s.formats[i].fourcc == DRM_FORMAT_ABGR8888 ||
                          s.formats[i].fourcc == DRM_FORMAT_XBGR8888;
            if (!is_rgb) continue;
            if (pass == 0 && s.formats[i].modifier != 0) continue;
            fourcc = s.formats[i].fourcc;
            modifier = s.formats[i].modifier;
            found = true;
            break;
        }
    }
    if (!found) {
        fprintf(stderr, "session 未提供可用的 RGB dmabuf 格式（共 %d 项）\n", s.n_formats);
        return 1;
    }
    fprintf(stderr, "[init] 选择格式 %s (%s), modifier=0x%llx\n",
            fourcc_name(fourcc), "fourcc", (unsigned long long)modifier);

    /* 分配 dmabuf */
    uint32_t flags = GBM_BO_USE_LINEAR;
    if (modifier != 0) flags = 0;
    s.bo = gbm_bo_create(s.gbm, s.width, s.height, fourcc, flags);
    if (!s.bo) {
        fprintf(stderr, "gbm_bo_create 失败 (%ux%u %s flags=0x%x)\n", s.width, s.height,
                fourcc_name(fourcc), flags);
        return 1;
    }
    s.stride = gbm_bo_get_stride(s.bo);
    s.dmabuf_fd = gbm_bo_get_fd(s.bo);
    if (s.dmabuf_fd < 0) {
        fprintf(stderr, "gbm_bo_get_fd 失败\n");
        return 1;
    }
    fprintf(stderr, "[init] gbm bo: stride=%u fd=%d bo_modifier=0x%llx\n",
            s.stride, s.dmabuf_fd, (unsigned long long)gbm_bo_get_modifier(s.bo));

    s.buffer = create_dmabuf_buffer(&s, s.dmabuf_fd, fourcc, s.width, s.height, s.stride, modifier);
    if (!s.buffer) {
        fprintf(stderr, "创建 wl_buffer 失败\n");
        return 1;
    }
    wl_buffer_add_listener(s.buffer, &buffer_listener, &s);

    fprintf(stderr, "[run] 开始抓帧，目标 %d 帧\n", s.frames_target);

    if (submit_frame(&s) != 0) {
        return 1;
    }

    while (s.running && wl_display_dispatch(s.display) != -1) {}

    int n = s.frames_done > 0 ? s.frames_done : 1;
    printf("\n===== 结果 =====\n");
    printf("成功帧数      : %d (失败 %d)\n", s.frames_done, s.frames_failed);
    printf("分辨率        : %ux%u\n", s.width, s.height);
    printf("单帧耗时(ms)  : avg %.3f / min %.3f / max %.3f\n",
           s.latency_sum_ms / n, s.latency_min_ms, s.latency_max_ms);
    printf("理论最大帧率  : %.1f fps\n", 1000.0 / (s.latency_sum_ms / n));

    gbm_bo_destroy(s.bo);
    gbm_device_destroy(s.gbm);
    wl_display_disconnect(s.display);
    return s.frames_done > 0 ? 0 : 1;
}
