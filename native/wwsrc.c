/*
 * wwsrc —— 测试用视频源
 *
 * 为什么需要它：本机的 Hyprland 在安全模式下无法被 screencopy 抓帧（一抓就崩），
 * 但"原生客户端 vs 浏览器"的对比只依赖同一条视频流。这里把一段预先用 NVENC
 * 编好的 H.264（High 4:4:4 / 全范围 / BT.709，参数与 wwcap 一致）按 60fps
 * 重新打包成 wwcap 的 24 字节分片格式，发给网关的 Unix socket。
 *
 * 这样浏览器和原生客户端拿到的是**逐字节相同**的流，对比结果只反映客户端差异，
 * 不受抓帧/合成器波动影响。
 *
 * 只用标准库：Annex-B 按 NAL 类型切访问单元，不依赖 libavformat。
 * 用法：
 *   wwsrc --file /tmp/clip.h264 --fps 60 --video /run/user/1000/ww-video.sock
 *         [--drop PCT] [--ctrl /run/user/1000/wwcap-ctrl.sock]
 *
 * 测试用例：docs/TEST-CASES.md 的「O. 原生客户端（wwclient）」（NAT-01~12）。
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define WW_VID_HDR_SIZE 24
#define WW_MAX_PAYLOAD 1100
#define KEY 0x01
#define FIRST 0x02
#define LAST 0x04

static volatile sig_atomic_t g_quit;

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static void on_sig(int s)
{
    (void)s;
    g_quit = 1;
}

struct au {
    size_t off;
    size_t len;
    bool key;
};

/* 扫描 Annex-B，按访问单元切分。
   规则：当前 AU 里已经出现过 VCL（1/5）后，再遇到 SPS/PPS/SEI/AUD（6/7/8/9）
   就从这里开新 AU —— NVENC 的 IDR 一定带 SPS/PPS，非 IDR 直接以 slice 开始。 */
static struct au *split_au(const uint8_t *d, size_t n, size_t *count)
{
    size_t cap = 1024, cnt = 0;
    struct au *aus = malloc(cap * sizeof(*aus));
    if (!aus) {
        return NULL;
    }
    size_t au_start = 0;
    bool has_vcl = false, has_idr = false, opened = false;

    for (size_t i = 0; i + 3 < n; i++) {
        if (d[i] != 0 || d[i + 1] != 0 || d[i + 2] != 1) {
            continue;
        }
        size_t sc = i;                     /* 起始码位置（00 00 01） */
        if (i > 0 && d[i - 1] == 0) {
            sc = i - 1;                    /* 四字节起始码 00 00 00 01 */
        }
        uint8_t type = d[i + 3] & 0x1f;

        if (type == 6 || type == 7 || type == 8 || type == 9) {
            if (has_vcl) {
                if (cnt == cap) {
                    cap *= 2;
                    struct au *t = realloc(aus, cap * sizeof(*aus));
                    if (!t) {
                        free(aus);
                        return NULL;
                    }
                    aus = t;
                }
                aus[cnt++] = (struct au){ au_start, sc - au_start, has_idr };
                au_start = sc;
                has_vcl = has_idr = false;
            }
            opened = true;
        } else if (type == 1 || type == 5) {
            if (type == 5) {
                has_idr = true;
            }
            has_vcl = true;
            opened = true;
        }
    }
    if (opened && au_start < n) {
        if (cnt == cap) {
            cap *= 2;
            struct au *t = realloc(aus, cap * sizeof(*aus));
            if (!t) {
                free(aus);
                return NULL;
            }
            aus = t;
        }
        aus[cnt++] = (struct au){ au_start, n - au_start, has_idr };
    }
    *count = cnt;
    return aus;
}

int main(int argc, char **argv)
{
    const char *file = NULL;
    const char *sock_path = "/run/user/1000/ww-video.sock";
    int fps = 60;
    double drop_pct = 0;   /* 每片独立丢弃概率（%），用于模拟弱网 */
    const char *ctrl_path = "/run/user/1000/wwcap-ctrl.sock";

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--file") && i + 1 < argc) file = argv[++i];
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--video") && i + 1 < argc) sock_path = argv[++i];
        else if (!strcmp(argv[i], "--drop") && i + 1 < argc) drop_pct = atof(argv[++i]);
        else if (!strcmp(argv[i], "--ctrl") && i + 1 < argc) ctrl_path = argv[++i];
        else {
            fprintf(stderr, "用法: wwsrc --file clip.h264 [--fps 60] [--video sock] [--drop PCT] [--ctrl sock]\n");
            return 2;
        }
    }
    if (!file) {
        fprintf(stderr, "--file 必填\n");
        return 2;
    }

    signal(SIGINT, on_sig);
    signal(SIGTERM, on_sig);

    FILE *fp = fopen(file, "rb");
    if (!fp) {
        perror(file);
        return 1;
    }
    struct stat st;
    fstat(fileno(fp), &st);
    uint8_t *data = malloc((size_t)st.st_size);
    if (!data || fread(data, 1, (size_t)st.st_size, fp) != (size_t)st.st_size) {
        fprintf(stderr, "读文件失败\n");
        return 1;
    }
    fclose(fp);

    size_t nau = 0;
    struct au *aus = split_au(data, (size_t)st.st_size, &nau);
    if (!aus || nau == 0) {
        fprintf(stderr, "没解析出访问单元（是 Annex-B 裸流吗？）\n");
        return 1;
    }
    fprintf(stderr, "[src] %s: %zu 个访问单元, %.1f MB\n", file, nau,
            (double)st.st_size / 1e6);

    int sock = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }
    struct sockaddr_un dst = { .sun_family = AF_UNIX };
    snprintf(dst.sun_path, sizeof(dst.sun_path), "%s", sock_path);

    /* 监听 wwcap 控制口：客户端丢片后会发 {"t":"kf"}，这里用「跳到下一个 IDR」
       模拟编码器被要求立刻出关键帧，从而验证整条 kf→恢复 链路。 */
    int ctrl_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (ctrl_fd >= 0) {
        struct sockaddr_un ca = { .sun_family = AF_UNIX };
        snprintf(ca.sun_path, sizeof(ca.sun_path), "%s", ctrl_path);
        unlink(ctrl_path);
        if (bind(ctrl_fd, (struct sockaddr *)&ca, sizeof(ca)) != 0) {
            fprintf(stderr, "[src] 控制口 %s 绑定失败，kf 请求不会生效\n", ctrl_path);
            close(ctrl_fd);
            ctrl_fd = -1;
        } else {
            fcntl(ctrl_fd, F_SETFL, fcntl(ctrl_fd, F_GETFL, 0) | O_NONBLOCK);
        }
    }

    uint8_t out[WW_VID_HDR_SIZE + WW_MAX_PAYLOAD];
    uint32_t frame_id = 0;
    uint64_t t0 = now_us();
    uint64_t sent = 0, bytes = 0;
    int64_t next_t = 0;
    size_t loop_i = 0;
    size_t last_key_i = 0;

    while (!g_quit) {
        /* 先看控制口：客户端报告丢片会立刻要关键帧 */
        if (ctrl_fd >= 0) {
            char cb[512];
            while (recv(ctrl_fd, cb, sizeof(cb) - 1, 0) > 0) {
                cb[sizeof(cb) - 1] = 0;
                if (strstr(cb, "\"kf\"")) {
                    /* 回到最近一个 IDR 重放：等价于真实编码器「立刻出一帧 IDR」，
                       且不会跳过内容（pts 用的是发送时钟，重放不会倒退）。 */
                    loop_i = last_key_i;
                    fprintf(stderr, "[src] kf -> 从最近的 IDR 重放（帧 %zu）\n", last_key_i);
                }
            }
        }
        struct au *a = &aus[loop_i];
        if (a->key) {
            last_key_i = loop_i;
        }
        const uint8_t *p = data + a->off;
        size_t remain = a->len;
        int cnt = (int)((remain + WW_MAX_PAYLOAD - 1) / WW_MAX_PAYLOAD);
        if (cnt == 0) {
            cnt = 1;
        }
        uint64_t pts = t0 + (uint64_t)(next_t * 1000000 / fps);
        for (int i = 0; i < cnt; i++) {
            size_t off = (size_t)i * WW_MAX_PAYLOAD;
            size_t len = remain - off;
            if (len > WW_MAX_PAYLOAD) {
                len = WW_MAX_PAYLOAD;
            }
            out[0] = 1;   /* version */
            out[1] = (uint8_t)((a->key ? KEY : 0) | (i == 0 ? FIRST : 0) |
                               (i == cnt - 1 ? LAST : 0));
            out[2] = 0;
            out[3] = 0;
            memcpy(out + 4, &frame_id, 4);
            uint16_t fi = (uint16_t)i, fc = (uint16_t)cnt;
            memcpy(out + 8, &fi, 2);
            memcpy(out + 10, &fc, 2);
            uint32_t pl = (uint32_t)len;
            memcpy(out + 12, &pl, 4);
            memcpy(out + 16, &pts, 8);
            memcpy(out + WW_VID_HDR_SIZE, p + off, len);
            if (drop_pct > 0 && (double)(rand() % 10000) < drop_pct * 100.0) {
                continue;   /* 模拟丢包：客户端应触发 kf 恢复 */
            }
            ssize_t r = sendto(sock, out, WW_VID_HDR_SIZE + len, 0,
                               (struct sockaddr *)&dst, sizeof(dst));
            (void)r;   /* 网关未起时 ENOENT 很正常，下一帧继续 */
        }
        bytes += remain;
        sent++;
        frame_id++;
        loop_i = (loop_i + 1) % nau;

        /* 绝对时间轴节拍：掉队时立即补发，不累积漂移 */
        next_t++;
        uint64_t target = t0 + (uint64_t)(next_t * 1000000 / fps);
        uint64_t now = now_us();
        if (target > now) {
            struct timespec ts = { .tv_sec = (time_t)((target - now) / 1000000),
                                   .tv_nsec = (long)(((target - now) % 1000000) * 1000) };
            nanosleep(&ts, NULL);
        } else if (now - target > 500000) {
            /* 落后超过 0.5s 说明链路停了，直接跳回当前时刻 */
            t0 += now - target;
        }

        if ((sent & 127) == 0) {
            double el = (double)(now_us() - t0) / 1e6;
            fprintf(stderr, "[src] 已发 %llu 帧, %.1f Mbps, %.1f fps\n",
                    (unsigned long long)sent,
                    el > 0 ? (double)bytes * 8.0 / el / 1e6 : 0, el > 0 ? (double)sent / el : 0);
        }
    }

    free(aus);
    free(data);
    close(sock);
    fprintf(stderr, "[src] 结束，共 %llu 帧\n", (unsigned long long)sent);
    return 0;
}
