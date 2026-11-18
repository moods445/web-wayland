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
    uint32_t mods;      /* XKB 修饰键位掩码：1=shift 4=ctrl 8=alt 64=super */
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
    case 58: bit = 2; break;               /* CAPSLOCK   */
    default: break;
    }
    if (bit) {
        if (down) in->mods |= bit; else in->mods &= ~bit;
        zwp_virtual_keyboard_v1_modifiers(in->kb, in->mods, 0, 0, 0);
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

    /* 用协议原生的绝对定位：不累积误差，点击位置与画面严格对应 */
    in->time_ms = now_ms();
    uint32_t x = (uint32_t)(nx * 65535.0 + 0.5);
    uint32_t y = (uint32_t)(ny * 65535.0 + 0.5);
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
    in->time_ms = now_ms();
    zwlr_virtual_pointer_v1_button(in->ptr, in->time_ms, button, down ? 1 : 0);
    zwlr_virtual_pointer_v1_frame(in->ptr);
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
