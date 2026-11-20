/*
 * 输入注入：zwp_virtual_keyboard_v1 + zwlr_virtual_pointer_v1
 * 走 Wayland 协议而不是 XTEST —— 这样 Wayland 原生应用（waybar/GTK4/fuzzel）也能收到事件。
 */

#define _GNU_SOURCE

#include "input.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <xkbcommon/xkbcommon.h>

#include "generated/virtual-keyboard-unstable-v1.h"
#include "generated/wlr-virtual-pointer-unstable-v1.h"

#define WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 1

struct ww_input {
    struct wl_display *dpy;
    struct zwp_virtual_keyboard_v1 *kb;
    struct zwlr_virtual_pointer_v1 *ptr;
    uint32_t time_ms;
    /* 绝对定位用「相对位移」实现：Hyprland 对 motion_absolute 的参数解释
       与本项目手写协议头不一致（实测 y 收到的是 x 值），故自行跟踪位置 */
    int width, height;
    double cur_x, cur_y;
    /* 抓帧输出在 compositor 布局里的位置/逻辑尺寸，以及整个布局的逻辑尺寸 */
    int out_x, out_y, out_w, out_h;
    int layout_w, layout_h;
    uint32_t mods;      /* XKB 修饰键位掩码：1=shift 4=ctrl 8=alt 64=super */
    uint32_t locked;    /* XKB locked 位：2=CapsLock */
    uint8_t pressed[256];  /* 已按下的键，断线时补发释放 */
    uint8_t buttons[8];    /* 已按下的鼠标键（BTN_LEFT=272 起） */
};

static void send_keymap(struct ww_input *in)
{
    struct xkb_context *ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!ctx) {
        return;
    }
    struct xkb_rule_names names = {
        .rules = NULL, .model = NULL, .layout = "us", .variant = NULL, .options = NULL,
    };
    struct xkb_keymap *km = xkb_keymap_new_from_names(ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!km) {
        xkb_context_unref(ctx);
        return;
    }
    char *str = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1);
    if (!str) {
        xkb_keymap_unref(km);
        xkb_context_unref(ctx);
        return;
    }
    size_t len = strlen(str) + 1;

    int fd = memfd_create("ww-keymap", MFD_CLOEXEC);
    if (fd < 0) {
        free(str);
        xkb_keymap_unref(km);
        xkb_context_unref(ctx);
        return;
    }
    if (write(fd, str, len) != (ssize_t)len) {
        fprintf(stderr, "[input] 写 keymap 失败\n");
    }
    /* 关键：fd 偏移是跨进程共享的，不归零会让 compositor 读到空 keymap */
    if (lseek(fd, 0, SEEK_SET) < 0) {
        fprintf(stderr, "[input] keymap lseek 失败: %s\n", strerror(errno));
    }
    zwp_virtual_keyboard_v1_keymap(in->kb, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd, (uint32_t)len);
    close(fd);

    free(str);
    xkb_keymap_unref(km);
    xkb_context_unref(ctx);
}

struct ww_input *ww_input_create(struct wl_display *dpy, struct wl_seat *seat)
{
    struct ww_input *in = calloc(1, sizeof(*in));
    if (!in) {
        return NULL;
    }
    in->dpy = dpy;
    /* 默认给 1x1：绝对定位本身用归一化坐标，宽高只用于有效性判断。
       以前初始为 0，导致首帧编码成功之前所有鼠标事件被静默丢弃。 */
    in->width = 1;
    in->height = 1;

    struct wl_registry *reg = wl_display_get_registry(dpy);
    (void)reg;

    /* 全局对象需由调用方提供已绑定的 manager；这里通过 registry 重新绑定代价高，
       因此改由 main 传入。为简洁，这里假定 main 已通过环境注入的 manager 指针。 */

    (void)seat;
    return in;
}

/* 由 main 直接注入已绑定的 manager（避免二次绑定 registry） */
void ww_input_attach(struct ww_input *in, struct zwp_virtual_keyboard_manager_v1 *kbmgr,
                     struct zwlr_virtual_pointer_manager_v1 *ptrmgr, struct wl_seat *seat)
{
    if (kbmgr && seat) {
        in->kb = zwp_virtual_keyboard_manager_v1_create_virtual_keyboard(kbmgr, seat);
        if (in->kb) {
            send_keymap(in);
        }
    }
    if (ptrmgr) {
        /* 传 NULL seat 让 compositor 用默认 seat */
        in->ptr = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(ptrmgr, seat);
    }
    fprintf(stderr, "[input] 虚拟键盘=%p 虚拟指针=%p\n", (void *)in->kb, (void *)in->ptr);
}

static uint32_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000u + ts.tv_nsec / 1000000u);
}

void ww_input_key(struct ww_input *in, uint32_t keycode, bool down)
{
    if (!in || !in->kb || keycode == 0) {
        return;
    }
    in->time_ms = now_ms();

    /* 维护修饰键状态：compositor 靠 modifiers 请求判定 SUPER/CTRL 等组合键 */
    uint32_t bit = 0;
    switch (keycode) {
    case 42: case 54: bit = 1; break;      /* SHIFT_L/R  */
    case 29: case 97: bit = 4; break;      /* CTRL_L/R   */
    case 56: case 100: bit = 8; break;     /* ALT_L/R    */
    case 125: case 126: bit = 64; break;   /* META_L/R   */
    default: break;
    }
    bool mods_changed = false;
    if (bit) {
        if (down) in->mods |= bit; else in->mods &= ~bit;
        mods_changed = true;
    }
    /* CapsLock 是锁定键：按下翻转 locked 位，松开不再清掉 */
    if (keycode == 58) {
        if (down) in->locked ^= 2;
        mods_changed = true;
    }
    if (mods_changed) {
        zwp_virtual_keyboard_v1_modifiers(in->kb, in->mods, 0, in->locked, 0);
    }
    if (keycode < 256) {
        in->pressed[keycode] = down ? 1 : 0;
    }

    /* Hyprland 内部会把收到的码 +8 转成 xkb keycode，因此这里直接发 evdev 原始码 */
    zwp_virtual_keyboard_v1_key(in->kb, in->time_ms, keycode, down ? 1 : 0);
    wl_display_flush(in->dpy);
}

void ww_input_set_geometry(struct ww_input *in, int w, int h)
{
    if (!in) {
        return;
    }
    in->width = w > 0 ? w : 1;
    in->height = h > 0 ? h : 1;
    in->cur_x = 0.5;
    in->cur_y = 0.5;
}

void ww_input_set_layout(struct ww_input *in, int ox, int oy, int ow, int oh,
                         int layout_w, int layout_h)
{
    if (!in) {
        return;
    }
    in->out_x = ox;
    in->out_y = oy;
    in->out_w = ow > 0 ? ow : 0;
    in->out_h = oh > 0 ? oh : 0;
    in->layout_w = layout_w > 0 ? layout_w : 0;
    in->layout_h = layout_h > 0 ? layout_h : 0;
}

void ww_input_set_position(struct ww_input *in, int px, int py)
{
    if (!in || in->width <= 0 || in->height <= 0) {
        return;
    }
    in->cur_x = (double)px / (double)in->width;
    in->cur_y = (double)py / (double)in->height;
}

void ww_input_motion_abs(struct ww_input *in, double nx, double ny)
{
    if (!in || !in->ptr || in->width <= 0) {
        return;
    }
    if (nx < 0) nx = 0;
    if (nx > 1) nx = 1;
    if (ny < 0) ny = 0;
    if (ny > 1) ny = 1;

    /* 用协议原生的绝对定位：不累积误差，点击位置与画面严格对应。
       但 motion_absolute 的 (x,y,extent) 是**整个布局**的归一化坐标：
       直接发 nx/ny 会把指针按布局宽度摊开（两块屏时视频里光标跑到另一块屏，
       点击位置也跟着错位）。先换算到抓帧输出在布局中的物理位置。 */
    double lw = in->layout_w > 0 ? (double)in->layout_w : (double)in->width;
    double lh = in->layout_h > 0 ? (double)in->layout_h : (double)in->height;
    double ow = in->out_w > 0 ? (double)in->out_w : (double)in->width;
    double oh = in->out_h > 0 ? (double)in->out_h : (double)in->height;
    double ax = ((double)in->out_x + nx * ow) / lw;
    double ay = ((double)in->out_y + ny * oh) / lh;
    if (ax < 0) ax = 0;
    if (ax > 1) ax = 1;
    if (ay < 0) ay = 0;
    if (ay > 1) ay = 1;

    in->time_ms = now_ms();
    uint32_t x = (uint32_t)(ax * 65535.0 + 0.5);
    uint32_t y = (uint32_t)(ay * 65535.0 + 0.5);
    zwlr_virtual_pointer_v1_motion_absolute(in->ptr, in->time_ms, x, y, 65535, 65535);
    zwlr_virtual_pointer_v1_frame(in->ptr);
    wl_display_flush(in->dpy);
}

void ww_input_motion_rel(struct ww_input *in, double dx, double dy)
{
    if (!in || !in->ptr) {
        return;
    }
    in->time_ms = now_ms();
    zwlr_virtual_pointer_v1_motion(in->ptr, in->time_ms,
                                   (wl_fixed_t)(dx * 256.0), (wl_fixed_t)(dy * 256.0));
    zwlr_virtual_pointer_v1_frame(in->ptr);
    wl_display_flush(in->dpy);
}

void ww_input_button(struct ww_input *in, uint32_t button, bool down)
{
    if (!in || !in->ptr) {
        return;
    }
    if (button >= 272 && button < 280) {
        in->buttons[button - 272] = down ? 1 : 0;
    }
    in->time_ms = now_ms();
    zwlr_virtual_pointer_v1_button(in->ptr, in->time_ms, button, down ? 1 : 0);
    zwlr_virtual_pointer_v1_frame(in->ptr);
    wl_display_flush(in->dpy);
}

/* 客户端断开/页面刷新时调用：补发所有释放，避免 SUPER/CTRL 卡死在桌面 */
void ww_input_reset(struct ww_input *in)
{
    if (!in) {
        return;
    }
    in->time_ms = now_ms();
    if (in->kb) {
        for (int i = 1; i < 256; i++) {
            if (in->pressed[i]) {
                zwp_virtual_keyboard_v1_key(in->kb, in->time_ms, (uint32_t)i, 0);
                in->pressed[i] = 0;
            }
        }
        in->mods = 0;
        in->locked = 0;
        zwp_virtual_keyboard_v1_modifiers(in->kb, 0, 0, 0, 0);
    }
    if (in->ptr) {
        for (int b = 0; b < 8; b++) {
            if (in->buttons[b]) {
                zwlr_virtual_pointer_v1_button(in->ptr, in->time_ms,
                                               (uint32_t)(272 + b), 0);
                in->buttons[b] = 0;
            }
        }
        zwlr_virtual_pointer_v1_frame(in->ptr);
    }
    wl_display_flush(in->dpy);
}

void ww_input_axis(struct ww_input *in, double value)
{
    if (!in || !in->ptr) {
        return;
    }
    in->time_ms = now_ms();
    zwlr_virtual_pointer_v1_axis_source(in->ptr, 0 /* wheel */);
    zwlr_virtual_pointer_v1_axis(in->ptr, in->time_ms, 0 /* vertical */,
                                 (wl_fixed_t)(value * 256.0));
    zwlr_virtual_pointer_v1_frame(in->ptr);
    wl_display_flush(in->dpy);
}

void ww_input_flush(struct ww_input *in)
{
    if (in && in->dpy) {
        wl_display_flush(in->dpy);
    }
}

void ww_input_destroy(struct ww_input *in)
{
    if (!in) {
        return;
    }
    if (in->kb) {
        zwp_virtual_keyboard_v1_destroy(in->kb);
    }
    if (in->ptr) {
        zwlr_virtual_pointer_v1_destroy(in->ptr);
    }
    free(in);
}
