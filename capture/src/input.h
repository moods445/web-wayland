#ifndef WW_INPUT_H
#define WW_INPUT_H

#include <stdbool.h>
#include <stdint.h>

struct wl_display;
struct wl_seat;
struct ww_input;
struct zwp_virtual_keyboard_manager_v1;
struct zwlr_virtual_pointer_manager_v1;

struct ww_input *ww_input_create(struct wl_display *dpy, struct wl_seat *seat);
void ww_input_attach(struct ww_input *in, struct zwp_virtual_keyboard_manager_v1 *kbmgr,
                     struct zwlr_virtual_pointer_manager_v1 *ptrmgr, struct wl_seat *seat);
void ww_input_destroy(struct ww_input *in);

/* keycode 为 Linux evdev keycode（前端直接发送） */
void ww_input_key(struct ww_input *in, uint32_t keycode, bool down);
/* 设定输出尺寸并初始化跟踪位置（绝对定位用它换算成相对位移） */
void ww_input_set_geometry(struct ww_input *in, int w, int h);
void ww_input_set_position(struct ww_input *in, int px, int py);
void ww_input_motion_abs(struct ww_input *in, double nx, double ny);
void ww_input_motion_rel(struct ww_input *in, double dx, double dy);
void ww_input_button(struct ww_input *in, uint32_t button, bool down);
void ww_input_axis(struct ww_input *in, double value);
void ww_input_flush(struct ww_input *in);

#endif
