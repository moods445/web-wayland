#ifndef WW_CAPTURE_SHM_H
#define WW_CAPTURE_SHM_H

#include <stdbool.h>
#include <stdint.h>

struct wl_display;
struct wl_output;
struct ww_shm;

/* 每成功抓到一帧就回调（data 为 BGRA，stride 为行字节数；w/h 为当前输出像素尺寸） */
typedef void (*ww_frame_cb)(void *ud, const void *data, uint32_t stride,
                            uint32_t w, uint32_t h, uint64_t pts_us);

/* 建 streaming 会话：内部自行绑定 zwlr_screencopy_manager_v1 与 wl_shm */
struct ww_shm *ww_shm_create(struct wl_display *dpy, struct wl_output *output,
                             int fps, ww_frame_cb cb, void *ud);

/* 在主循环里按帧率节流地发起下一次抓帧 */
void ww_shm_tick(struct ww_shm *s);

/* 是否已有抓帧在飞行中（用于精确测量抓帧延迟） */
bool ww_shm_pending(struct ww_shm *s);

/* 运行时改采集帧率（客户端设置面板用） */
void ww_shm_set_fps(struct ww_shm *s, int fps);

/* 抓帧往返平均耗时（毫秒），调用后清零 */
double ww_shm_take_cap_ms(struct ww_shm *s);

/* 是否已协商到可用格式 */
bool ww_shm_ready(struct ww_shm *s);

void ww_shm_destroy(struct ww_shm *s);

#endif
