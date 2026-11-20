/*
 * wwclient —— web-wayland 原生客户端
 *
 * 目标：验证"不做浏览器，直接做一个原生客户端"能换来什么。
 *  - 绕过 WebCodecs：H.264 High 4:4:4 在 Chrome 里根本解不了，这里用 FFmpeg 软解
 *  - 绕过 WebTransport/QUIC：直连网关的 8444/udp，少一层发送队列
 *  - 绕开 4:2:0/limited：按服务端 VUI（BT.709 全范围）用 GLSL 直接 YUV→RGB，
 *    色度不降采样、黑白不压范围，桌面文字和纯色块能对上本地屏幕
 *
 * 上屏路径：FFmpeg 解出 yuv444p / yuv420p 三平面 → OpenGL 三张 GL_R8 纹理
 *           → 全屏 quad 着色器做矩阵转换（GPU，几乎零成本）
 *
 * 用法：
 *   wwclient --host 127.0.0.1 --port 8444 [--size 1920x1080] [--fullscreen]
 *            [--no-vsync] [--no-444] [--bench SEC] [--dump-frame out.ppm]
 *            [--dump-gl out.ppm] [--dump-after SEC] [--stats-json out.json]
 *
 * 测试用例与判定标准：docs/TEST-CASES.md 的「O. 原生客户端（wwclient）」（NAT-01~12）。
 * 收益数据与对照结论：docs/native-client-assessment.md。
 */

#define _GNU_SOURCE

#include <SDL2/SDL.h>
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

/* 与 proto/wire.h 保持一致（这里不直接 include，避免把 capture 的依赖带进来） */
#define WW_VID_HDR_SIZE 24
#define WW_MAX_PAYLOAD 1100
#define WW_FRAG_KEY 0x01
#define WW_FRAG_FIRST 0x02
#define WW_FRAG_LAST 0x04

#define NAT_HDR 8
#define T_VIDEO 1
#define T_AUDIO 2
#define T_CTRL 3
#define T_CTRL_UP 4
#define T_HELLO 5
#define T_HELLO_ACK 6
#define T_PING 7
#define T_PONG 8
#define T_STATS 9

#define MAX_FRAME_BYTES (16u * 1024u * 1024u)
#define MAX_FRAGS 16384

static volatile sig_atomic_t g_quit;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static double now_ms(void)
{
    return (double)now_us() / 1000.0;
}

/* ── 参数 ─────────────────────────────────────────────────────── */
struct opts {
    const char *host;
    int port;
    int win_w, win_h;
    bool fullscreen;
    bool vsync;
    bool want444;
    bool no_present;        /* 无显示环境：只解码，不建 GL 窗口 */
    double bench_sec;       /* >0：跑够时间自动退出 */
    const char *dump_frame;
    double dump_after;      /* 解码满 N 秒后落盘一帧 */
    const char *dump_gl;    /* 落盘 GL 上屏后的实际像素（校验着色器色彩） */
    const char *stats_json;
};

/* ── 指标 ─────────────────────────────────────────────────────── */
struct metrics {
    uint64_t frames, dropped, bad_frags, undecodable, skipped;
    double decode_ms_sum, present_ms_sum, e2e_ms_sum;
    uint64_t win_frames, win_start_us;
    double decode_ms[2048];
    double present_ms[2048];
    double e2e_ms[2048];
    int n;
    double rtt_ms;
};

static int cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}

static double pct(double *v, int n, double p)
{
    if (n <= 0) {
        return 0;
    }
    qsort(v, (size_t)n, sizeof(double), cmp_double);
    int i = (int)(p * (double)(n - 1) + 0.5);
    if (i < 0) i = 0;
    if (i >= n) i = n - 1;
    return v[i];
}

/* ── 帧重组 ───────────────────────────────────────────────────── */
struct reasm {
    uint8_t *buf;
    uint8_t *seen;
    uint32_t frame_id;
    uint16_t frag_count;
    uint16_t got;
    uint64_t pts_us;
    uint32_t bytes;
    bool active;
};

static bool reasm_start(struct reasm *r, uint32_t frame_id, uint16_t count,
                        uint64_t pts_us)
{
    if (count == 0 || count > MAX_FRAGS) {
        return false;
    }
    if (!r->buf) {
        r->buf = malloc(MAX_FRAME_BYTES);
        r->seen = malloc(MAX_FRAGS);
    }
    if (!r->buf || !r->seen) {
        return false;
    }
    memset(r->seen, 0, count);
    r->frame_id = frame_id;
    r->frag_count = count;
    r->got = 0;
    r->bytes = 0;
    r->pts_us = pts_us;
    r->active = true;
    return true;
}

/* 返回 true 表示收齐了一个完整访问单元 */
static bool reasm_frag(struct reasm *r, uint32_t frame_id, uint16_t idx,
                       uint16_t count, uint32_t len, uint64_t pts_us,
                       const uint8_t *payload, uint16_t payload_len)
{
    if (!r->active || frame_id != r->frame_id || count != r->frag_count ||
        idx >= count) {
        return false;
    }
    if (len > WW_MAX_PAYLOAD || payload_len < len) {
        return false;
    }
    size_t off = (size_t)idx * WW_MAX_PAYLOAD;
    if (off + len > MAX_FRAME_BYTES) {
        return false;
    }
    /* 重复分片（UDP 极端乱序/重传）直接忽略，否则 got 会虚高 */
    if (r->seen[idx]) {
        return false;
    }
    r->seen[idx] = 1;
    memcpy(r->buf + off, payload, len);
    r->got++;
    if (len > 0) {
        size_t end = off + len;
        if (end > r->bytes) {
            r->bytes = (uint32_t)end;
        }
    }
    r->pts_us = pts_us;
    return r->got == r->frag_count;
}

/* ── OpenGL ───────────────────────────────────────────────────── */
struct glctx {
    SDL_Window *win;
    SDL_GLContext ctx;
    GLuint prog, vao, vbo;
    GLuint tex[3];
    int tex_w[3], tex_h[3];
    int uni_y, uni_u, uni_v, uni_444, uni_full, uni_709;
    int vid_w, vid_h;
    bool is444;
};

static const char *VS_SRC =
    "#version 330 core\n"
    "layout(location=0) in vec2 a_pos;\n"
    "out vec2 v_uv;\n"
    "void main(){ v_uv = vec2(a_pos.x*0.5+0.5, 0.5 - a_pos.y*0.5);\n"
    "  gl_Position = vec4(a_pos, 0.0, 1.0); }\n";

static const char *FS_SRC =
    "#version 330 core\n"
    "in vec2 v_uv; out vec4 frag;\n"
    "uniform sampler2D u_y, u_u, u_v;\n"
    "uniform int u_444, u_full, u_709;\n"
    "void main(){\n"
    "  float y = texture(u_y, v_uv).r;\n"
    "  float u = texture(u_u, v_uv).r;\n"
    "  float v = texture(u_v, v_uv).r;\n"
    "  if (u_full == 0) {\n"
    "    y = (y - 16.0/255.0) * (255.0/219.0);\n"
    "    u = (u - 128.0/255.0) * (255.0/224.0);\n"
    "    v = (v - 128.0/255.0) * (255.0/224.0);\n"
    "  } else {\n"
    "    u -= 128.0/255.0; v -= 128.0/255.0;\n"
    "  }\n"
    "  vec3 rgb;\n"
    "  if (u_709 == 1) rgb = vec3(y + 1.5748*v, y - 0.1873*u - 0.4681*v,\n"
    "                             y + 1.8556*u);\n"
    "  else rgb = vec3(y + 1.402*v, y - 0.344136*u - 0.714136*v, y + 1.772*u);\n"
    "  frag = vec4(clamp(rgb, 0.0, 1.0), 1.0);\n"
    "}\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "[gl] 着色器编译失败: %s\n", log);
    }
    return s;
}

static int gl_init(struct glctx *g, const struct opts *o)
{
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE;
    if (o->fullscreen) {
        flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    }
    g->win = SDL_CreateWindow("wwclient", SDL_WINDOWPOS_CENTERED,
                              SDL_WINDOWPOS_CENTERED, o->win_w, o->win_h, flags);
    if (!g->win) {
        fprintf(stderr, "[sdl] 窗口创建失败: %s\n", SDL_GetError());
        return -1;
    }
    g->ctx = SDL_GL_CreateContext(g->win);
    if (!g->ctx) {
        fprintf(stderr, "[gl] 上下文创建失败: %s\n", SDL_GetError());
        return -1;
    }
    SDL_GL_SetSwapInterval(o->vsync ? 1 : 0);
    fprintf(stderr, "[gl] %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    GLuint vs = compile(GL_VERTEX_SHADER, VS_SRC);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FS_SRC);
    g->prog = glCreateProgram();
    glAttachShader(g->prog, vs);
    glAttachShader(g->prog, fs);
    glLinkProgram(g->prog);
    GLint ok = 0;
    glGetProgramiv(g->prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        glGetProgramInfoLog(g->prog, sizeof(log), NULL, log);
        fprintf(stderr, "[gl] 链接失败: %s\n", log);
        return -1;
    }
    glDeleteShader(vs);
    glDeleteShader(fs);

    static const float quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glGenVertexArrays(1, &g->vao);
    glBindVertexArray(g->vao);
    glGenBuffers(1, &g->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float), (void *)0);

    glGenTextures(3, g->tex);
    for (int i = 0; i < 3; i++) {
        glBindTexture(GL_TEXTURE_2D, g->tex[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        g->tex_w[i] = g->tex_h[i] = 0;
    }
    g->uni_y = glGetUniformLocation(g->prog, "u_y");
    g->uni_u = glGetUniformLocation(g->prog, "u_u");
    g->uni_v = glGetUniformLocation(g->prog, "u_v");
    g->uni_444 = glGetUniformLocation(g->prog, "u_444");
    g->uni_full = glGetUniformLocation(g->prog, "u_full");
    g->uni_709 = glGetUniformLocation(g->prog, "u_709");
    return 0;
}

static void plane_upload(GLuint tex, int *tw, int *th, int w, int h,
                         const uint8_t *data, int linesize)
{
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, linesize);
    if (*tw != w || *th != h) {
        glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, w, h, 0, GL_RED,
                     GL_UNSIGNED_BYTE, data);
        *tw = w;
        *th = h;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RED,
                        GL_UNSIGNED_BYTE, data);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

/* 把上屏区域读回来落成 PPM：没有显示器时唯一能验证「着色器输出的颜色对不对」的办法。
   必须在 SwapWindow 之前读，交换后后备缓冲内容未定义。 */
static int gl_readback(int x, int y, int w, int h, const char *path)
{
    if (w <= 0 || h <= 0) {
        return -1;
    }
    uint8_t *rgba = malloc((size_t)w * h * 4);
    if (!rgba) {
        return -1;
    }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fprintf(fp, "P6\n%d %d\n255\n", w, h);
        uint8_t *row = malloc((size_t)w * 3);
        for (int yy = h - 1; yy >= 0; yy--) {          /* GL 原点在左下 */
            const uint8_t *s = rgba + (size_t)yy * w * 4;
            for (int xx = 0; xx < w; xx++) {
                row[xx * 3 + 0] = s[xx * 4 + 0];
                row[xx * 3 + 1] = s[xx * 4 + 1];
                row[xx * 3 + 2] = s[xx * 4 + 2];
            }
            fwrite(row, 1, (size_t)w * 3, fp);
        }
        free(row);
        fclose(fp);
    }
    free(rgba);
    return fp ? 0 : -1;
}

static void gl_present(struct glctx *g, const AVFrame *f, bool full, bool is709,
                       struct metrics *m, const char *dump_gl)
{
    int w = f->width, h = f->height;
    bool is444 = f->format == AV_PIX_FMT_YUV444P || f->format == AV_PIX_FMT_YUVJ444P;
    int cw = is444 ? w : (w + 1) / 2;
    int ch = is444 ? h : (h + 1) / 2;

    plane_upload(g->tex[0], &g->tex_w[0], &g->tex_h[0], w, h, f->data[0], f->linesize[0]);
    plane_upload(g->tex[1], &g->tex_w[1], &g->tex_h[1], cw, ch, f->data[1], f->linesize[1]);
    plane_upload(g->tex[2], &g->tex_w[2], &g->tex_h[2], cw, ch, f->data[2], f->linesize[2]);
    g->vid_w = w;
    g->vid_h = h;
    g->is444 = is444;

    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(g->win, &ww, &wh);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    /* 保持宽高比：letterbox。视频区域的矩形用于把鼠标坐标映射回画面归一化坐标 */
    double sx = (double)ww / (double)w;
    double sy = (double)wh / (double)h;
    double scale = sx < sy ? sx : sy;
    double dw = (double)w * scale;
    double dh = (double)h * scale;
    int vx = (int)(((double)ww - dw) * 0.5);
    int vy = (int)(((double)wh - dh) * 0.5);
    glViewport(vx, vy, (GLsizei)dw, (GLsizei)dh);

    glUseProgram(g->prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g->tex[0]);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, g->tex[1]);
    glActiveTexture(GL_TEXTURE2);
    glBindTexture(GL_TEXTURE_2D, g->tex[2]);
    glUniform1i(g->uni_y, 0);
    glUniform1i(g->uni_u, 1);
    glUniform1i(g->uni_v, 2);
    glUniform1i(g->uni_444, is444 ? 1 : 0);
    glUniform1i(g->uni_full, full ? 1 : 0);
    glUniform1i(g->uni_709, is709 ? 1 : 0);
    glBindVertexArray(g->vao);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    if (dump_gl) {
        glFinish();
        if (gl_readback(vx, vy, (int)dw, (int)dh, dump_gl) == 0) {
            fprintf(stderr, "[dump] GL 帧已落盘 %s（%dx%d）\n", dump_gl, (int)dw, (int)dh);
        }
    }

    uint64_t t0 = now_us();
    SDL_GL_SwapWindow(g->win);
    if (m) {
        m->present_ms_sum += (double)(now_us() - t0) / 1000.0;
    }
}

/* ── 网络发送 ─────────────────────────────────────────────────── */
static int g_sock = -1;
static struct sockaddr_in g_dst;
static uint32_t g_seq;

static void nat_send(uint8_t type, const void *payload, size_t len)
{
    uint8_t buf[NAT_HDR + 8192];
    if (len > sizeof(buf) - NAT_HDR) {
        return;
    }
    buf[0] = 'W';
    buf[1] = 'N';
    buf[2] = type;
    buf[3] = 0;
    uint32_t seq = ++g_seq;
    memcpy(buf + 4, &seq, 4);
    if (len) {
        memcpy(buf + NAT_HDR, payload, len);
    }
    sendto(g_sock, buf, NAT_HDR + len, 0, (struct sockaddr *)&g_dst, sizeof(g_dst));
}

static void nat_ctrl(const char *json)
{
    nat_send(T_CTRL_UP, json, strlen(json));
}

/* 鼠标/键盘的坐标与按钮格式与浏览器完全一致，服务端只需要一套注入逻辑 */
static double g_vx = -1, g_vy = -1;
static uint64_t g_last_move_us;

static void send_move(double nx, double ny)
{
    if (nx < 0) nx = 0;
    if (nx > 1) nx = 1;
    if (ny < 0) ny = 0;
    if (ny > 1) ny = 1;
    g_vx = nx;
    g_vy = ny;
    char b[160];
    g_last_move_us = now_us();
    snprintf(b, sizeof(b), "{\"t\":\"pm\",\"x\":%.5f,\"y\":%.5f,\"ts\":%.1f}",
             nx, ny, now_ms());
    nat_ctrl(b);
}

/* SDL scancode → Linux evdev keycode（虚拟键盘协议要的就是 evdev 码） */
static int sdl_to_evdev(SDL_Scancode sc)
{
    switch (sc) {
    case SDL_SCANCODE_ESCAPE: return 1;
    case SDL_SCANCODE_1: return 2;
    case SDL_SCANCODE_2: return 3;
    case SDL_SCANCODE_3: return 4;
    case SDL_SCANCODE_4: return 5;
    case SDL_SCANCODE_5: return 6;
    case SDL_SCANCODE_6: return 7;
    case SDL_SCANCODE_7: return 8;
    case SDL_SCANCODE_8: return 9;
    case SDL_SCANCODE_9: return 10;
    case SDL_SCANCODE_0: return 11;
    case SDL_SCANCODE_MINUS: return 12;
    case SDL_SCANCODE_EQUALS: return 13;
    case SDL_SCANCODE_BACKSPACE: return 14;
    case SDL_SCANCODE_TAB: return 15;
    case SDL_SCANCODE_Q: return 16;
    case SDL_SCANCODE_W: return 17;
    case SDL_SCANCODE_E: return 18;
    case SDL_SCANCODE_R: return 19;
    case SDL_SCANCODE_T: return 20;
    case SDL_SCANCODE_Y: return 21;
    case SDL_SCANCODE_U: return 22;
    case SDL_SCANCODE_I: return 23;
    case SDL_SCANCODE_O: return 24;
    case SDL_SCANCODE_P: return 25;
    case SDL_SCANCODE_LEFTBRACKET: return 26;
    case SDL_SCANCODE_RIGHTBRACKET: return 27;
    case SDL_SCANCODE_RETURN: return 28;
    case SDL_SCANCODE_LCTRL: return 29;
    case SDL_SCANCODE_A: return 30;
    case SDL_SCANCODE_S: return 31;
    case SDL_SCANCODE_D: return 32;
    case SDL_SCANCODE_F: return 33;
    case SDL_SCANCODE_G: return 34;
    case SDL_SCANCODE_H: return 35;
    case SDL_SCANCODE_J: return 36;
    case SDL_SCANCODE_K: return 37;
    case SDL_SCANCODE_L: return 38;
    case SDL_SCANCODE_SEMICOLON: return 39;
    case SDL_SCANCODE_APOSTROPHE: return 40;
    case SDL_SCANCODE_GRAVE: return 41;
    case SDL_SCANCODE_LSHIFT: return 42;
    case SDL_SCANCODE_BACKSLASH: return 43;
    case SDL_SCANCODE_Z: return 44;
    case SDL_SCANCODE_X: return 45;
    case SDL_SCANCODE_C: return 46;
    case SDL_SCANCODE_V: return 47;
    case SDL_SCANCODE_B: return 48;
    case SDL_SCANCODE_N: return 49;
    case SDL_SCANCODE_M: return 50;
    case SDL_SCANCODE_COMMA: return 51;
    case SDL_SCANCODE_PERIOD: return 52;
    case SDL_SCANCODE_SLASH: return 53;
    case SDL_SCANCODE_RSHIFT: return 54;
    case SDL_SCANCODE_KP_MULTIPLY: return 55;
    case SDL_SCANCODE_LALT: return 56;
    case SDL_SCANCODE_SPACE: return 57;
    case SDL_SCANCODE_CAPSLOCK: return 58;
    case SDL_SCANCODE_F1: return 59;
    case SDL_SCANCODE_F2: return 60;
    case SDL_SCANCODE_F3: return 61;
    case SDL_SCANCODE_F4: return 62;
    case SDL_SCANCODE_F5: return 63;
    case SDL_SCANCODE_F6: return 64;
    case SDL_SCANCODE_F7: return 65;
    case SDL_SCANCODE_F8: return 66;
    case SDL_SCANCODE_F9: return 67;
    case SDL_SCANCODE_F10: return 68;
    case SDL_SCANCODE_NUMLOCKCLEAR: return 69;
    case SDL_SCANCODE_SCROLLLOCK: return 70;
    case SDL_SCANCODE_KP_7: return 71;
    case SDL_SCANCODE_KP_8: return 72;
    case SDL_SCANCODE_KP_9: return 73;
    case SDL_SCANCODE_KP_MINUS: return 74;
    case SDL_SCANCODE_KP_4: return 75;
    case SDL_SCANCODE_KP_5: return 76;
    case SDL_SCANCODE_KP_6: return 77;
    case SDL_SCANCODE_KP_PLUS: return 78;
    case SDL_SCANCODE_KP_1: return 79;
    case SDL_SCANCODE_KP_2: return 80;
    case SDL_SCANCODE_KP_3: return 81;
    case SDL_SCANCODE_KP_0: return 82;
    case SDL_SCANCODE_KP_PERIOD: return 83;
    case SDL_SCANCODE_F11: return 87;
    case SDL_SCANCODE_F12: return 88;
    case SDL_SCANCODE_KP_ENTER: return 96;
    case SDL_SCANCODE_RCTRL: return 97;
    case SDL_SCANCODE_KP_DIVIDE: return 98;
    case SDL_SCANCODE_SYSREQ: return 99;
    case SDL_SCANCODE_RALT: return 100;
    case SDL_SCANCODE_HOME: return 102;
    case SDL_SCANCODE_UP: return 103;
    case SDL_SCANCODE_PAGEUP: return 104;
    case SDL_SCANCODE_LEFT: return 105;
    case SDL_SCANCODE_RIGHT: return 106;
    case SDL_SCANCODE_END: return 107;
    case SDL_SCANCODE_DOWN: return 108;
    case SDL_SCANCODE_PAGEDOWN: return 109;
    case SDL_SCANCODE_INSERT: return 110;
    case SDL_SCANCODE_DELETE: return 111;
    case SDL_SCANCODE_MUTE: return 113;
    case SDL_SCANCODE_VOLUMEDOWN: return 114;
    case SDL_SCANCODE_VOLUMEUP: return 115;
    case SDL_SCANCODE_LGUI: return 125;
    case SDL_SCANCODE_RGUI: return 126;
    case SDL_SCANCODE_APPLICATION: return 127;
    default: return -1;
    }
}

static int button_to_evdev(int b)
{
    switch (b) {
    case SDL_BUTTON_LEFT: return 272;
    case SDL_BUTTON_RIGHT: return 273;
    case SDL_BUTTON_MIDDLE: return 274;
    case SDL_BUTTON_X1: return 275;
    case SDL_BUTTON_X2: return 276;
    default: return 0;
    }
}

/* 窗口坐标 → 视频内容归一化坐标（考虑 letterbox） */
static void mouse_to_video(struct glctx *g, int mx, int my, double *nx, double *ny)
{
    int ww = 0, wh = 0;
    SDL_GL_GetDrawableSize(g->win, &ww, &wh);
    int vw = g->vid_w ? g->vid_w : 1920;
    int vh = g->vid_h ? g->vid_h : 1080;
    double sx = (double)ww / vw, sy = (double)wh / vh;
    double s = sx < sy ? sx : sy;
    double dw = vw * s, dh = vh * s;
    double ox = (ww - dw) * 0.5, oy = (wh - dh) * 0.5;
    *nx = ((double)mx - ox) / dw;
    *ny = ((double)my - oy) / dh;
}

/* ── 解码器 ───────────────────────────────────────────────────── */
static AVCodecContext *g_dec;
static AVFrame *g_frame;

static int dec_init(void)
{
    const AVCodec *c = avcodec_find_decoder(AV_CODEC_ID_H264);
    if (!c) {
        fprintf(stderr, "[dec] 找不到 H.264 解码器\n");
        return -1;
    }
    g_dec = avcodec_alloc_context3(c);
    g_dec->flags |= AV_CODEC_FLAG_LOW_DELAY;
    /* 切片级多线程：不会像帧级线程那样把帧压在半路，对串流延迟更友好 */
    g_dec->thread_type = FF_THREAD_SLICE;
    g_dec->thread_count = 4;
    if (avcodec_open2(g_dec, c, NULL) < 0) {
        fprintf(stderr, "[dec] 解码器打开失败\n");
        return -1;
    }
    g_frame = av_frame_alloc();
    return 0;
}

/* 把解码帧写成 PPM，用于和服务端 grim 截图做 PSNR/ΔE 对比 */
static int dump_ppm(const AVFrame *f, const char *path, bool full, bool is709)
{
    struct SwsContext *sws = sws_getContext(
        f->width, f->height, (enum AVPixelFormat)f->format,
        f->width, f->height, AV_PIX_FMT_RGB24, SWS_BILINEAR, NULL, NULL, NULL);
    if (!sws) {
        return -1;
    }
    /* YUV→RGB 的矩阵与范围必须和后端一致，否则测出来的偏差是假的 */
    const int *coef = sws_getCoefficients(is709 ? SWS_CS_ITU709 : SWS_CS_ITU601);
    sws_setColorspaceDetails(sws, coef, full ? 1 : 0, coef, 1, 0, 1 << 16, 1 << 16);

    uint8_t *rgb[4] = { NULL };
    int stride[4] = { 0 };
    if (av_image_alloc(rgb, stride, f->width, f->height, AV_PIX_FMT_RGB24, 1) < 0) {
        sws_freeContext(sws);
        return -1;
    }
    sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height,
              rgb, stride);
    FILE *fp = fopen(path, "wb");
    if (fp) {
        fprintf(fp, "P6\n%d %d\n255\n", f->width, f->height);
        for (int y = 0; y < f->height; y++) {
            fwrite(rgb[0] + (size_t)y * stride[0], 1, (size_t)f->width * 3, fp);
        }
        fclose(fp);
    }
    av_freep(&rgb[0]);
    sws_freeContext(sws);
    return fp ? 0 : -1;
}

/* ── 主循环 ───────────────────────────────────────────────────── */
/* 丢片后立刻要一个 IDR：UDP 没有重传，等周期 IDR（gop=120，60fps 下 2 秒）
   画面会一直花到下一个关键帧。限速 100ms，避免弱网下把 kf 打成风暴。 */
static uint64_t g_last_kf_us;
static bool g_need_key = true;   /* 还没等到 IDR：跳过错帧，避免把花屏画上去 */

static void request_keyframe(void)
{
    uint64_t n = now_us();
    if (n - g_last_kf_us < 100000) {
        return;
    }
    g_last_kf_us = n;
    nat_ctrl("{\"t\":\"kf\"}");
}

static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

int main(int argc, char **argv)
{
    struct opts o = {
        .host = "127.0.0.1",
        .port = 8444,
        .win_w = 1600,
        .win_h = 900,
        .vsync = true,
        .want444 = true,
        .dump_after = 2.0,
    };
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) o.host = argv[++i];
        else if (!strcmp(argv[i], "--port") && i + 1 < argc) o.port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) {
            sscanf(argv[++i], "%dx%d", &o.win_w, &o.win_h);
        } else if (!strcmp(argv[i], "--fullscreen")) o.fullscreen = true;
        else if (!strcmp(argv[i], "--no-vsync")) o.vsync = false;
        else if (!strcmp(argv[i], "--no-444")) o.want444 = false;
        else if (!strcmp(argv[i], "--no-present")) o.no_present = true;
        else if (!strcmp(argv[i], "--bench") && i + 1 < argc) o.bench_sec = atof(argv[++i]);
        else if (!strcmp(argv[i], "--dump-frame") && i + 1 < argc) o.dump_frame = argv[++i];
        else if (!strcmp(argv[i], "--dump-after") && i + 1 < argc) o.dump_after = atof(argv[++i]);
        else if (!strcmp(argv[i], "--dump-gl") && i + 1 < argc) o.dump_gl = argv[++i];
        else if (!strcmp(argv[i], "--stats-json") && i + 1 < argc) o.stats_json = argv[++i];
        else {
            fprintf(stderr,
                    "用法: wwclient [--host H] [--port P] [--size WxH] [--fullscreen]\n"
                    "       [--no-vsync] [--no-444] [--no-present] [--bench SEC]\n"
                    "       [--dump-frame FILE] [--dump-gl FILE] [--dump-after SEC] [--stats-json FILE]\n");
            return 2;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    g_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_sock < 0) {
        perror("socket");
        return 1;
    }
    memset(&g_dst, 0, sizeof(g_dst));
    g_dst.sin_family = AF_INET;
    g_dst.sin_port = htons((uint16_t)o.port);
    if (inet_pton(AF_INET, o.host, &g_dst.sin_addr) != 1) {
        fprintf(stderr, "无效地址 %s\n", o.host);
        return 1;
    }
    int fl = fcntl(g_sock, F_GETFL, 0);
    fcntl(g_sock, F_SETFL, fl | O_NONBLOCK);
    int rcvbuf = 8 * 1024 * 1024;
    setsockopt(g_sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    if (SDL_Init(o.no_present ? 0 : SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "[sdl] 初始化失败: %s\n", SDL_GetError());
        return 1;
    }
    struct glctx g;
    memset(&g, 0, sizeof(g));
    if (!o.no_present && gl_init(&g, &o) != 0) {
        return 1;
    }
    if (dec_init() != 0) {
        return 1;
    }

    struct reasm ra;
    memset(&ra, 0, sizeof(ra));
    struct metrics m;
    memset(&m, 0, sizeof(m));
    m.win_start_us = now_us();

    bool full_range = true;
    bool is709 = true;
    bool hello_done = false;
    uint64_t t_start = now_us();
    uint64_t t_last_hello = 0, t_last_ping = 0, t_last_report = 0;
    uint64_t t_first_frame = 0;
    bool dumped = false;
    bool dumped_gl = false;
    double rtt_ms = 0;

    /* 首帧之前先发 hello，网关会立刻回 caps 并强制一个 IDR */
    nat_send(T_HELLO, "wwclient", 8);
    t_last_hello = now_us();
    /* 原生端默认请求"桌面级"参数：4:4:4 + 独显编码。
       wwcap 若只有集显会自动回退，caps 里会如实反映。 */
    if (o.want444) {
        nat_ctrl("{\"t\":\"chroma\",\"v\":1}");
    }
    nat_ctrl("{\"t\":\"enc\",\"v\":\"nvenc\"}");

    int running = 1;
    while (running && !g_quit) {
        uint64_t now = now_us();
        if (o.bench_sec > 0 && (double)(now - t_start) / 1e6 >= o.bench_sec) {
            break;
        }
        if (now - t_last_hello > 1000000) {
            nat_send(T_HELLO, "wwclient", 8);
            t_last_hello = now;
        }
        if (now - t_last_ping > 1000000) {
            uint32_t s = ++g_seq;
            nat_send(T_PING, &s, 4);
            t_last_ping = now;
        }

        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) {
                running = 0;
            } else if (ev.type == SDL_MOUSEMOTION) {
                double nx, ny;
                mouse_to_video(&g, ev.motion.x, ev.motion.y, &nx, &ny);
                send_move(nx, ny);
            } else if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
                int b = button_to_evdev(ev.button.button);
                if (b) {
                    char j[96];
                    snprintf(j, sizeof(j), "{\"t\":\"pb\",\"b\":%d,\"d\":%d}", b,
                             ev.type == SDL_MOUSEBUTTONDOWN ? 1 : 0);
                    nat_ctrl(j);
                }
            } else if (ev.type == SDL_MOUSEWHEEL) {
                char j[96];
                snprintf(j, sizeof(j), "{\"t\":\"pw\",\"v\":%.1f}", ev.wheel.y * 10.0);
                nat_ctrl(j);
            } else if (ev.type == SDL_KEYDOWN || ev.type == SDL_KEYUP) {
                int k = sdl_to_evdev(ev.key.keysym.scancode);
                if (k > 0) {
                    char j[96];
                    snprintf(j, sizeof(j), "{\"t\":\"kk\",\"k\":%d,\"d\":%d}", k,
                             ev.type == SDL_KEYDOWN ? 1 : 0);
                    nat_ctrl(j);
                }
            }
        }

        /* 收包：一次最多处理 256 个，避免持续高码率时事件处理饿死 */
        uint8_t pkt[2048];
        for (int iter = 0; iter < 256; iter++) {
            struct sockaddr_in from;
            socklen_t fl2 = sizeof(from);
            ssize_t n = recvfrom(g_sock, pkt, sizeof(pkt), 0,
                                 (struct sockaddr *)&from, &fl2);
            if (n < 0) {
                break;
            }
            if (n < NAT_HDR || pkt[0] != 'W' || pkt[1] != 'N') {
                continue;
            }
            uint8_t type = pkt[2];
            const uint8_t *pl = pkt + NAT_HDR;
            size_t pln = (size_t)n - NAT_HDR;
            if (type == T_VIDEO) {
                if (pln < WW_VID_HDR_SIZE) {
                    m.bad_frags++;
                    continue;
                }
                uint8_t ver = pl[0];
                uint8_t flags = pl[1];
                uint32_t frame_id;
                uint16_t fidx, fcnt;
                uint32_t paylen;
                uint64_t pts;
                memcpy(&frame_id, pl + 4, 4);
                memcpy(&fidx, pl + 8, 2);
                memcpy(&fcnt, pl + 10, 2);
                memcpy(&paylen, pl + 12, 4);
                memcpy(&pts, pl + 16, 8);
                if (ver != 1 || paylen > WW_MAX_PAYLOAD ||
                    pln < WW_VID_HDR_SIZE + paylen) {
                    m.bad_frags++;
                    continue;
                }
                if (flags & WW_FRAG_FIRST) {
                    if (ra.active && ra.got < ra.frag_count) {
                        m.dropped++;
                        g_need_key = true;
                        request_keyframe();
                    }
                    if (!ra.active || ra.frame_id != frame_id ||
                        ra.frag_count != fcnt) {
                        if (!reasm_start(&ra, frame_id, fcnt, pts)) {
                            m.bad_frags++;
                            continue;
                        }
                    }
                }
                bool complete = reasm_frag(&ra, frame_id, fidx, fcnt, paylen, pts,
                                           pl + WW_VID_HDR_SIZE, (uint16_t)paylen);
                if (!complete) {
                    continue;
                }
                ra.active = false;
                if (ra.bytes == 0) {
                    continue;
                }
                /* 丢过片就等 IDR：中间的 P 帧参考了没收全的帧，解出来也是花屏 */
                if (g_need_key) {
                    if (!(flags & WW_FRAG_KEY)) {
                        m.skipped++;
                        continue;
                    }
                    g_need_key = false;
                }

                AVPacket *ap = av_packet_alloc();
                av_new_packet(ap, (int)ra.bytes);
                memcpy(ap->data, ra.buf, ra.bytes);
                ap->pts = (int64_t)ra.pts_us;

                uint64_t t0 = now_us();
                if (avcodec_send_packet(g_dec, ap) == 0) {
                    while (avcodec_receive_frame(g_dec, g_frame) == 0) {
                        uint64_t t1 = now_us();
                        double dec_ms = (double)(t1 - t0) / 1000.0;
                        g.vid_w = g_frame->width;
                        g.vid_h = g_frame->height;
                        g.is444 = g_frame->format == AV_PIX_FMT_YUV444P ||
                                  g_frame->format == AV_PIX_FMT_YUVJ444P;
                        if (!o.no_present) {
                            bool gl_dump = o.dump_gl && !dumped_gl && t_first_frame &&
                                           (double)(t1 - t_first_frame) / 1e6 >= o.dump_after;
                            gl_present(&g, g_frame, full_range, is709, &m,
                                       gl_dump ? o.dump_gl : NULL);
                            if (gl_dump) {
                                dumped_gl = true;
                                running = 0;
                            }
                        }
                        uint64_t t2 = now_us();
                        /* pts 是服务端 CLOCK_MONOTONIC：同机测试可直接相减，
                           跨机时该值只反映时钟差，报告里会注明 */
                        double e2e = (double)(t2 - (uint64_t)g_frame->pts) / 1000.0;
                        m.frames++;
                        m.decode_ms_sum += dec_ms;
                        m.e2e_ms_sum += e2e;
                        m.win_frames++;
                        if (m.n < (int)(sizeof(m.decode_ms) / sizeof(m.decode_ms[0]))) {
                            m.decode_ms[m.n] = dec_ms;
                            m.present_ms[m.n] = (double)(t2 - t1) / 1000.0;
                            m.e2e_ms[m.n] = e2e;
                            m.n++;
                        }
                        if (!t_first_frame) {
                            t_first_frame = t2;
                        }
                        /* 画质采样：等画面稳定（默认 2 秒）后把当前解码帧落盘。
                           此时脚本同时用 grim 截服务端输出，两张图做 PSNR/ΔE。 */
                        if (o.dump_frame && !dumped && t_first_frame &&
                            (double)(t2 - t_first_frame) / 1e6 >= o.dump_after) {
                            if (dump_ppm(g_frame, o.dump_frame, full_range, is709) == 0) {
                                fprintf(stderr, "[dump] 已落盘 %s（%dx%d %s）\n",
                                        o.dump_frame, g_frame->width, g_frame->height,
                                        g.is444 ? "4:4:4" : "4:2:0");
                            }
                            dumped = true;
                            running = 0;   /* 采完即退出，脚本不用猜结束时刻 */
                        }
                        av_frame_unref(g_frame);
                        t0 = now_us();
                    }
                } else {
                    m.undecodable++;
                    request_keyframe();
                }
                av_packet_free(&ap);
            } else if (type == T_HELLO_ACK) {
                if (!hello_done && pln) {
                    hello_done = true;
                    char *tmp = malloc(pln + 1);
                    if (tmp) {
                        memcpy(tmp, pl, pln);
                        tmp[pln] = 0;
                        fprintf(stderr, "[net] 服务端就绪: %s\n", tmp);
                        if (strstr(tmp, "\"range\":\"full\"")) {
                            full_range = true;
                        } else if (strstr(tmp, "\"range\":\"limited\"")) {
                            full_range = false;
                        }
                        if (strstr(tmp, "\"color\":\"601\"")) {
                            is709 = false;
                        } else if (strstr(tmp, "\"color\":\"709\"")) {
                            is709 = true;
                        }
                        free(tmp);
                    }
                }
            } else if (type == T_PONG) {
                if (pln >= 4) {
                    rtt_ms = (double)(now_us() - t_last_ping) / 1000.0;
                }
            }
        }

        if (now - t_last_report > 2000000) {
            double span = (double)(now - t_last_report) / 1e6;
            if (m.n > 0) {
                double dp50 = pct(m.decode_ms, m.n, 0.50);
                double dp95 = pct(m.decode_ms, m.n, 0.95);
                double ep50 = pct(m.e2e_ms, m.n, 0.50);
                double ep95 = pct(m.e2e_ms, m.n, 0.95);
                fprintf(stderr,
                        "[net] fps=%.1f decode p50=%.2f p95=%.2f ms | e2e p50=%.2f p95=%.2f ms"
                        " | drop=%llu badfrag=%llu rtt=%.2f ms chroma=%s range=%s\n",
                        (double)m.win_frames / span, dp50, dp95, ep50, ep95,
                        (unsigned long long)m.dropped, (unsigned long long)m.bad_frags,
                        rtt_ms, g.is444 ? "4:4:4" : "4:2:0", full_range ? "full" : "limited");
            }
            m.win_frames = 0;
            /* 不重置 m.n：汇总统计覆盖整场测试，而不是最后一个 2 秒窗口 */
            t_last_report = now;
        }

        (void)t_last_hello;
        struct pollfd pfd = { .fd = g_sock, .events = POLLIN };
        poll(&pfd, 1, 2);
    }

    /* 汇总：给测试脚本直接消费的 NDJSON */
    double total_s = t_first_frame ? (double)(now_us() - t_first_frame) / 1e6 : 0;
    double dp50 = pct(m.decode_ms, m.n, 0.50);
    double dp95 = pct(m.decode_ms, m.n, 0.95);
    double pp50 = pct(m.present_ms, m.n, 0.50);
    double pp95 = pct(m.present_ms, m.n, 0.95);
    double ep50 = pct(m.e2e_ms, m.n, 0.50);
    double ep95 = pct(m.e2e_ms, m.n, 0.95);
    const char *chroma = g.is444 ? "444" : "420";
    printf("{\"type\":\"summary\",\"frames\":%llu,\"fps\":%.2f,"
           "\"decode_p50_ms\":%.2f,\"decode_p95_ms\":%.2f,"
           "\"present_p50_ms\":%.2f,\"present_p95_ms\":%.2f,"
           "\"e2e_p50_ms\":%.2f,\"e2e_p95_ms\":%.2f,"
           "\"rtt_ms\":%.2f,\"dropped\":%llu,\"bad_frags\":%llu,"
           "\"undecodable\":%llu,\"skipped\":%llu,\"width\":%d,\"height\":%d,"
           "\"chroma\":\"%s\",\"range\":\"%s\",\"color\":\"%s\"}\n",
           (unsigned long long)m.frames, total_s > 0 ? (double)m.frames / total_s : 0,
           dp50, dp95, pp50, pp95, ep50, ep95, rtt_ms,
           (unsigned long long)m.dropped, (unsigned long long)m.bad_frags,
           (unsigned long long)m.undecodable, (unsigned long long)m.skipped,
           g.vid_w, g.vid_h, chroma,
           full_range ? "full" : "limited", is709 ? "709" : "601");
    if (o.stats_json) {
        FILE *f = fopen(o.stats_json, "w");
        if (f) {
            fprintf(f, "{\"frames\":%llu,\"fps\":%.2f,\"decode_p50_ms\":%.2f,"
                       "\"decode_p95_ms\":%.2f,\"present_p50_ms\":%.2f,"
                       "\"present_p95_ms\":%.2f,\"e2e_p50_ms\":%.2f,"
                       "\"e2e_p95_ms\":%.2f,\"dropped\":%llu,\"bad_frags\":%llu,"
                       "\"undecodable\":%llu,\"skipped\":%llu,\"width\":%d,\"height\":%d,"
                       "\"chroma\":\"%s\",\"range\":\"%s\",\"color\":\"%s\"}\n",
                    (unsigned long long)m.frames,
                    total_s > 0 ? (double)m.frames / total_s : 0,
                    dp50, dp95, pp50, pp95, ep50, ep95,
                    (unsigned long long)m.dropped, (unsigned long long)m.bad_frags,
                    (unsigned long long)m.undecodable, (unsigned long long)m.skipped,
                    g.vid_w, g.vid_h, chroma,
                    full_range ? "full" : "limited", is709 ? "709" : "601");
            fclose(f);
        }
    }

    /* --dump-frame 已在解码循环里处理；这里不再需要额外的帧副本 */

    av_frame_free(&g_frame);
    avcodec_free_context(&g_dec);
    free(ra.buf);
    free(ra.seen);
    if (!o.no_present) {
        SDL_GL_DeleteContext(g.ctx);
        SDL_DestroyWindow(g.win);
    }
    SDL_Quit();
    close(g_sock);
    return 0;
}
