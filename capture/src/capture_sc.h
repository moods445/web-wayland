#ifndef WW_CAPTURE_SC_H
#define WW_CAPTURE_SC_H

#include <stdbool.h>
#include <stdint.h>

struct wl_display;
struct wl_output;
struct ww_sc;

/*
 * 零拷贝抓帧（独显专用）：
 *   wlr-screencopy dmabuf(tiled) → EGLImage → GL 纹理 → CUDA array
 *   → ARGB→NV12 kernel → pinned 主机内存
 *
 * y/uv 指向 pinned 主机内存，w/h 为像素尺寸；conv_ms 为本次 GPU 转换 + 下载耗时。
 */
typedef void (*ww_sc_frame_cb)(void *ud, const uint8_t *y, uint32_t ys,
                               const uint8_t *uv, uint32_t uvs,
                               uint32_t w, uint32_t h, uint64_t pts_us,
                               double conv_ms);

/* gbm_node: 与 compositor 同卡的 DRM 节点（独显会话为 /dev/dri/card0） */
struct ww_sc *ww_sc_create(struct wl_display *dpy, struct wl_output *output,
                           const char *gbm_node, int fps,
                           ww_sc_frame_cb cb, void *ud);

/* 主循环里按帧率节流地发起下一次抓帧 */
void ww_sc_tick(struct ww_sc *s);

/* 是否已成功抓到过至少一帧 */
bool ww_sc_ready(struct ww_sc *s);

/* 运行时改采集帧率 */
void ww_sc_set_fps(struct ww_sc *s, int fps);

/* 抓帧往返平均耗时（毫秒），调用后清零 */
double ww_sc_take_cap_ms(struct ww_sc *s);

void ww_sc_destroy(struct ww_sc *s);

#endif /* WW_CAPTURE_SC_H */
