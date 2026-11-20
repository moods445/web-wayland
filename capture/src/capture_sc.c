/*
 * 零拷贝抓帧后端（独显）：wlr-screencopy dmabuf(tiled) → EGLImage → GL shader → NV12
 *
 * 数据流：
 *   1. screencopy 把画面拷进 tiled gbm BO（compositor 同卡，GPU 直写）
 *   2. 每帧重建 EGLImage 包住 dmabuf，绑成 GL 纹理（复用旧 image 会冻结在首帧）
 *   3. 两趟全屏 shader：ARGB 纹理 → Y(R8) / UV(RG8+2x2 下采样)，全程在显存
 *   4. glReadPixels 经 PBO 异步下载 12.4MB NV12，fence 由主循环轮询
 *
 * 为什么不用 CUDA：cuGraphicsGLRegisterImage + cuGraphicsMapResources 在 4K
 * tiled 纹理上要把整帧 detile，实测 18~26ms/帧；常驻 map 又只能看到首帧快照。
 * GL 采样 tiled 纹理是原生的，shader 转换 + 回读只要几毫秒。
 *
 * 流水线：compositor 每个刷新周期只服务一个 screencopy，所以同时压 3~4 个
 * 请求在飞，吞吐才等于刷新率；回读用 PBO + fence，不能同步 glReadPixels
 * （同步回读每帧多花 12ms，帧率掉到 45）。
 */

#define _GNU_SOURCE
#define EGL_EGLEXT_PROTOTYPES 1
#define GL_GLEXT_PROTOTYPES 1

#include "capture_sc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include <gbm.h>
#include <drm_fourcc.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GL/gl.h>
#include <GL/glext.h>

#include "generated/wlr-screencopy-unstable-v1.h"
#include "generated/linux-dmabuf-v1.h"

/* 槽位数：compositor 每刷新周期只服务一个请求，队列要压 3~4 个才能跑满 60；
 * 多留一个给转换/回读占用 */
#define WW_SC_SLOTS 5

enum { SLOT_FREE = 0, SLOT_PENDING = 1, SLOT_CAPTURED = 2 };

struct sc_slot {
    struct gbm_bo *bo;
    struct wl_buffer *wl_buf;
    int fd;
    uint32_t w, h, stride, fmt;
    uint64_t modifier;

    EGLImageKHR img;
    GLuint src_tex;     /* EGLImage 绑定的采样纹理 */
    GLuint tex_y;       /* Y 平面渲染目标（R8） */
    GLuint tex_uv;      /* UV 平面渲染目标（RG8，半尺寸） */
    GLuint fbo;
    GLuint pbo;         /* 异步回读用的 PIXEL_PACK 缓冲 */
    GLsync fence;
    int pending_submit;
    uint64_t pts_us;
    double conv_ms;

    int state;
    uint64_t req_us;
};

struct sc_req {
    struct ww_sc *s;
    int slot;
    uint64_t req_us;
    struct zwlr_screencopy_frame_v1 *frame;
};

struct ww_sc {
    struct wl_display *dpy;
    struct wl_output *output;

    struct zwlr_screencopy_manager_v1 *mgr;
    struct zwp_linux_dmabuf_v1 *dmabuf;
    struct gbm_device *gbm;
    int gbm_fd;

    struct sc_slot slots[WW_SC_SLOTS];
    int next_slot;
    struct sc_req *reqs[WW_SC_SLOTS];   /* 每个槽位在飞行中的抓帧请求 */
    int inflight;

    int fps;
    uint64_t period_us;
    uint64_t last_req_us;
    bool ready;

    uint64_t sum_cap_us, n_cap;

    /* EGL / GL */
    EGLDisplay edpy;
    EGLContext ectx;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC gl_image_target;
    PFNEGLCREATEIMAGEKHRPROC egl_create_image;
    PFNEGLDESTROYIMAGEKHRPROC egl_destroy_image;
    PFNGLGENFRAMEBUFFERSPROC gl_gen_fbos;
    PFNGLDELETEFRAMEBUFFERSPROC gl_del_fbos;
    PFNGLBINDFRAMEBUFFERPROC gl_bind_fbo;
    PFNGLFRAMEBUFFERTEXTURE2DPROC gl_fbo_tex;

    GLuint prog_y, prog_uv;
    GLint u_y_src, u_uv_src;

    PFNGLFENCESYNCPROC gl_fence_sync;
    PFNGLCLIENTWAITSYNCPROC gl_client_wait_sync;
    PFNGLDELETESYNCPROC gl_delete_sync;
    PFNGLMAPBUFFERRANGEPROC gl_map_buffer_range;
    PFNGLUNMAPBUFFERPROC gl_unmap_buffer;

    /* 已提交回读、等待 fence 的槽位，按提交顺序出帧，保证帧序 */
    int pend_q[WW_SC_SLOTS];
    int pend_n;

    uint8_t *h_nv12;
    size_t nv12_size;
    uint32_t gpu_w, gpu_h;

    ww_sc_frame_cb cb;
    void *ud;
    int fail_logged;
};

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

static const char *glerr_str(GLenum e)
{
    switch (e) {
    case GL_NO_ERROR: return "none";
    case GL_INVALID_ENUM: return "INVALID_ENUM";
    case GL_INVALID_VALUE: return "INVALID_VALUE";
    case GL_INVALID_OPERATION: return "INVALID_OPERATION";
    case GL_OUT_OF_MEMORY: return "OUT_OF_MEMORY";
    case GL_INVALID_FRAMEBUFFER_OPERATION: return "INVALID_FRAMEBUFFER_OPERATION";
    default: return "?";
    }
}

/* ── registry（第二套：只取 screencopy 与 linux-dmabuf）─────────── */
static void reg_global(void *data, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t ver)
{
    struct ww_sc *s = data;
    if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name)) {
        s->mgr = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface,
                                  ver >= 3 ? 3 : ver);
    } else if (!strcmp(iface, zwp_linux_dmabuf_v1_interface.name)) {
        s->dmabuf = wl_registry_bind(reg, name, &zwp_linux_dmabuf_v1_interface,
                                     ver >= 3 ? 3 : ver);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener reg_listener = { .global = reg_global, .global_remove = reg_remove };

/* ── 尺寸变化时释放全部槽位与内存 ─────────────────────────────── */
static void slot_teardown(struct ww_sc *s, int idx)
{
    struct sc_slot *sl = &s->slots[idx];
    if (sl->fbo) {
        s->gl_del_fbos(1, &sl->fbo);
        sl->fbo = 0;
    }
    if (sl->fence) {
        s->gl_delete_sync(sl->fence);
        sl->fence = NULL;
    }
    if (sl->pbo) {
        glDeleteBuffers(1, &sl->pbo);
        sl->pbo = 0;
    }
    sl->pending_submit = 0;
    if (sl->tex_y) {
        glDeleteTextures(1, &sl->tex_y);
        sl->tex_y = 0;
    }
    if (sl->tex_uv) {
        glDeleteTextures(1, &sl->tex_uv);
        sl->tex_uv = 0;
    }
    if (sl->src_tex) {
        glDeleteTextures(1, &sl->src_tex);
        sl->src_tex = 0;
    }
    if (sl->img) {
        if (s->egl_destroy_image) {
            s->egl_destroy_image(s->edpy, sl->img);
        }
        sl->img = EGL_NO_IMAGE_KHR;
    }
    if (sl->wl_buf) {
        wl_buffer_destroy(sl->wl_buf);
        sl->wl_buf = NULL;
    }
    if (sl->fd >= 0) {
        close(sl->fd);
        sl->fd = -1;
    }
    if (sl->bo) {
        gbm_bo_destroy(sl->bo);
        sl->bo = NULL;
    }
    sl->state = SLOT_FREE;
}

static void free_gpu_buffers(struct ww_sc *s)
{
    free(s->h_nv12);
    s->h_nv12 = NULL;
    s->nv12_size = 0;
    s->gpu_w = s->gpu_h = 0;
}

static int alloc_gpu_buffers(struct ww_sc *s, uint32_t w, uint32_t h)
{
    if (s->gpu_w == w && s->gpu_h == h && s->h_nv12) {
        return 0;
    }
    free_gpu_buffers(s);

    s->nv12_size = (size_t)w * h * 3 / 2;
    s->h_nv12 = malloc(s->nv12_size);
    if (!s->h_nv12) {
        fprintf(stderr, "[sc] NV12 主机缓冲分配失败（%zu 字节）\n", s->nv12_size);
        return -1;
    }
    s->gpu_w = w;
    s->gpu_h = h;
    return 0;
}

/* ── 槽位构建：gbm BO + wl_buffer + GL 采样/渲染目标 ── */
static int slot_build(struct ww_sc *s, int idx, uint32_t fmt, uint32_t w, uint32_t h)
{
    struct sc_slot *sl = &s->slots[idx];
    if (sl->bo && sl->w == w && sl->h == h && sl->fmt == fmt) {
        return 0;
    }

    /* 尺寸变化：所有槽位一起重建，避免残留旧 BO */
    if (s->gpu_w != w || s->gpu_h != h || sl->bo) {
        s->pend_n = 0;
        for (int i = 0; i < WW_SC_SLOTS; i++) {
            slot_teardown(s, i);
        }
        if (alloc_gpu_buffers(s, w, h) != 0) {
            return -1;
        }
    }

    sl->bo = gbm_bo_create(s->gbm, w, h, fmt, GBM_BO_USE_RENDERING);
    if (!sl->bo) {
        fprintf(stderr, "[sc] gbm_bo_create 失败 %ux%u fmt=0x%x\n", w, h, fmt);
        return -1;
    }
    sl->w = w;
    sl->h = h;
    sl->fmt = fmt;
    sl->stride = gbm_bo_get_stride(sl->bo);
    sl->modifier = gbm_bo_get_modifier(sl->bo);
    sl->fd = gbm_bo_get_fd(sl->bo);
    if (sl->fd < 0) {
        fprintf(stderr, "[sc] gbm_bo_get_fd 失败\n");
        slot_teardown(s, idx);
        return -1;
    }

    struct zwp_linux_buffer_params_v1 *p = zwp_linux_dmabuf_v1_create_params(s->dmabuf);
    zwp_linux_buffer_params_v1_add(p, sl->fd, 0, 0, sl->stride,
                                   (uint32_t)(sl->modifier >> 32),
                                   (uint32_t)(sl->modifier & 0xffffffffu));
    sl->wl_buf = zwp_linux_buffer_params_v1_create_immed(p, w, h, fmt, 0);
    zwp_linux_buffer_params_v1_destroy(p);
    if (!sl->wl_buf) {
        fprintf(stderr, "[sc] wl_buffer 创建失败\n");
        slot_teardown(s, idx);
        return -1;
    }

    fprintf(stderr, "[sc] 槽位%d 缓冲就绪 %ux%u fmt=0x%x stride=%u modifier=0x%llx\n",
            idx, w, h, fmt, sl->stride, (unsigned long long)sl->modifier);
    return 0;
}

/* 全屏三角形，靠 gl_VertexID 生成顶点，不需要 VBO */
static const char *VS_SRC =
    "#version 130\n"
    "void main(){\n"
    "  vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));\n"
    "  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);\n"
    "}\n";

/* Y 平面：BT.601 limited，与 libyuv ARGBToI420 的整数系数一致 */
static const char *FS_Y_SRC =
    "#version 130\n"
    "uniform sampler2D u_src;\n"
    "out vec4 o;\n"
    "void main(){\n"
    "  vec4 c = texelFetch(u_src, ivec2(gl_FragCoord.xy), 0);\n"
    "  float r = c.r * 255.0, g = c.g * 255.0, b = c.b * 255.0;\n"
    "  float y = ((66.0 * r + 129.0 * g + 25.0 * b + 128.0) / 256.0) + 16.0;\n"
    "  o = vec4(clamp(y, 16.0, 235.0) / 255.0, 0.0, 0.0, 1.0);\n"
    "}\n";

/* UV 平面：每输出像素取源 2x2 平均 */
static const char *FS_UV_SRC =
    "#version 130\n"
    "uniform sampler2D u_src;\n"
    "out vec4 o;\n"
    "void main(){\n"
    "  ivec2 p = ivec2(gl_FragCoord.xy) * 2;\n"
    "  vec4 c = (texelFetch(u_src, p, 0) + texelFetch(u_src, p + ivec2(1, 0), 0) +\n"
    "            texelFetch(u_src, p + ivec2(0, 1), 0) + texelFetch(u_src, p + ivec2(1, 1), 0)) * 0.25;\n"
    "  float r = c.r * 255.0, g = c.g * 255.0, b = c.b * 255.0;\n"
    "  float u = ((-38.0 * r - 74.0 * g + 112.0 * b + 128.0) / 256.0) + 128.0;\n"
    "  float v = ((112.0 * r - 94.0 * g - 18.0 * b + 128.0) / 256.0) + 128.0;\n"
    "  o = vec4(clamp(u, 16.0, 240.0) / 255.0, clamp(v, 16.0, 240.0) / 255.0, 0.0, 1.0);\n"
    "}\n";

static GLuint compile_prog(const char *fs_src, const char *name)
{
    GLuint vs = glCreateShader(GL_VERTEX_SHADER);
    glShaderSource(vs, 1, &VS_SRC, NULL);
    glCompileShader(vs);
    GLint ok = 0;
    glGetShaderiv(vs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(vs, sizeof(log), NULL, log);
        fprintf(stderr, "[sc] %s 顶点着色器编译失败: %s\n", name, log);
        return 0;
    }
    GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(fs, 1, &fs_src, NULL);
    glCompileShader(fs);
    glGetShaderiv(fs, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(fs, sizeof(log), NULL, log);
        fprintf(stderr, "[sc] %s 片元着色器编译失败: %s\n", name, log);
        return 0;
    }
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glLinkProgram(p);
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    glDeleteShader(vs);
    glDeleteShader(fs);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(p, sizeof(log), NULL, log);
        fprintf(stderr, "[sc] %s 链接失败: %s\n", name, log);
        return 0;
    }
    return p;
}

/* 槽位的 GL 对象：采样纹理 + 两块渲染目标 + FBO（只建一次） */
static int slot_gl_init(struct ww_sc *s, int idx)
{
    struct sc_slot *sl = &s->slots[idx];
    if (sl->fbo) {
        return 0;
    }
    glGenTextures(1, &sl->src_tex);
    glBindTexture(GL_TEXTURE_2D, sl->src_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenTextures(1, &sl->tex_y);
    glBindTexture(GL_TEXTURE_2D, sl->tex_y);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, (GLsizei)sl->w, (GLsizei)sl->h, 0,
                 GL_RED, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    glGenTextures(1, &sl->tex_uv);
    glBindTexture(GL_TEXTURE_2D, sl->tex_uv);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RG8, (GLsizei)(sl->w / 2), (GLsizei)(sl->h / 2), 0,
                 GL_RG, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);

    s->gl_gen_fbos(1, &sl->fbo);
    s->gl_bind_fbo(GL_FRAMEBUFFER, sl->fbo);
    s->gl_fbo_tex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sl->tex_y, 0);
    GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    s->gl_bind_fbo(GL_FRAMEBUFFER, 0);
    if (st != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "[sc] FBO 不完整 0x%x\n", st);
        return -1;
    }

    glGenBuffers(1, &sl->pbo);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, sl->pbo);
    glBufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)s->nv12_size, NULL, GL_STREAM_READ);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        fprintf(stderr, "[sc] 槽位%d GL 初始化错误: %s\n", idx, glerr_str(e));
        return -1;
    }
    return 0;
}

/* ── 每帧转换：重建 EGLImage → shader 出 Y/UV → PBO 异步回读 ──
 *
 * 必须每帧重建 EGLImage：复用同一张 image 时，后续写入不会被看到
 * （实测画面冻结在挂接那一刻，换背景色完全无变化）。
 *
 * 回读走 PBO + fence，不在 GL 线程里同步等 DMA：同步 glReadPixels 会把
 * 帧率压到 45fps（每帧多花 12ms），改成「先提交、下一帧再取」即恢复 60fps。 */
static int convert_slot(struct ww_sc *s, int idx)
{
    struct sc_slot *sl = &s->slots[idx];
    if (!s->h_nv12 || !sl->bo) {
        return -1;
    }
    if (slot_gl_init(s, idx) != 0) {
        return -1;
    }
    uint64_t t0 = now_us();

    if (sl->img != EGL_NO_IMAGE_KHR) {
        s->egl_destroy_image(s->edpy, sl->img);
        sl->img = EGL_NO_IMAGE_KHR;
    }
    EGLint attribs[] = {
        EGL_LINUX_DRM_FOURCC_EXT, (EGLint)sl->fmt,
        EGL_WIDTH, (EGLint)sl->w,
        EGL_HEIGHT, (EGLint)sl->h,
        EGL_DMA_BUF_PLANE0_FD_EXT, sl->fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)sl->stride,
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, (EGLint)(sl->modifier & 0xffffffffu),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, (EGLint)(sl->modifier >> 32),
        EGL_NONE
    };
    sl->img = s->egl_create_image(s->edpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                  NULL, attribs);
    if (sl->img == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "[sc] eglCreateImage 失败 0x%x\n", eglGetError());
        return -1;
    }
    glBindTexture(GL_TEXTURE_2D, sl->src_tex);
    s->gl_image_target(GL_TEXTURE_2D, (GLeglImageOES)sl->img);
    GLenum e = glGetError();
    if (e != GL_NO_ERROR) {
        fprintf(stderr, "[sc] glEGLImageTargetTexture2DOES 失败: %s\n", glerr_str(e));
        return -1;
    }

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, sl->src_tex);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    s->gl_bind_fbo(GL_FRAMEBUFFER, sl->fbo);

    /* 两次回读都写进同一块 PBO：Y 在偏移 0，UV 紧随其后 */
    glBindBuffer(GL_PIXEL_PACK_BUFFER, sl->pbo);
    GLsizei yw = (GLsizei)sl->w, yh = (GLsizei)sl->h;

    /* Y */
    s->gl_fbo_tex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sl->tex_y, 0);
    glViewport(0, 0, yw, yh);
    glUseProgram(s->prog_y);
    glUniform1i(s->u_y_src, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(0, 0, yw, yh, GL_RED, GL_UNSIGNED_BYTE, (void *)(uintptr_t)0);

    /* UV */
    s->gl_fbo_tex(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, sl->tex_uv, 0);
    glViewport(0, 0, yw / 2, yh / 2);
    glUseProgram(s->prog_uv);
    glUniform1i(s->u_uv_src, 0);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glReadPixels(0, 0, yw / 2, yh / 2, GL_RG, GL_UNSIGNED_BYTE,
                 (void *)(uintptr_t)((size_t)sl->w * sl->h));

    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glUseProgram(0);
    s->gl_bind_fbo(GL_FRAMEBUFFER, 0);

    sl->fence = s->gl_fence_sync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
    e = glGetError();
    if (e != GL_NO_ERROR) {
        fprintf(stderr, "[sc] 转换 GL 错误: %s\n", glerr_str(e));
        return -1;
    }

    sl->conv_ms = (double)(now_us() - t0) / 1000.0;
    sl->pending_submit = 1;
    return 0;
}

/* ── 取回一张已完成的回读：PBO → 主机 NV12 → 回调 ──────────── */
static int harvest_one(struct ww_sc *s, int idx)
{
    struct sc_slot *sl = &s->slots[idx];
    if (!sl->pending_submit) {
        return 0;
    }
    /* 非阻塞探测：没读完就下一轮再来 */
    GLenum r = s->gl_client_wait_sync(sl->fence, 0, 0);
    if (r != GL_ALREADY_SIGNALED && r != GL_CONDITION_SATISFIED) {
        return 0;
    }

    int ok = 0;
    glBindBuffer(GL_PIXEL_PACK_BUFFER, sl->pbo);
    void *p = s->gl_map_buffer_range(GL_PIXEL_PACK_BUFFER, 0,
                                     (GLsizeiptr)s->nv12_size, GL_MAP_READ_BIT);
    if (p) {
        memcpy(s->h_nv12, p, s->nv12_size);
        s->gl_unmap_buffer(GL_PIXEL_PACK_BUFFER);
        ok = 1;
    } else {
        fprintf(stderr, "[sc] glMapBufferRange 失败\n");
    }
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);

    if (sl->fence) {
        s->gl_delete_sync(sl->fence);
        sl->fence = NULL;
    }
    sl->pending_submit = 0;
    sl->state = SLOT_FREE;

    if (ok && s->cb) {
        s->cb(s->ud, s->h_nv12, s->gpu_w,
              s->h_nv12 + (size_t)s->gpu_w * s->gpu_h, s->gpu_w,
              s->gpu_w, s->gpu_h, sl->pts_us, sl->conv_ms);
    }
    return 1;
}

/* 按提交顺序出帧：队头的 fence 未 signal 就停下来，避免乱序 */
static void drain_pending(struct ww_sc *s)
{
    while (s->pend_n > 0) {
        int idx = s->pend_q[0];
        if (!harvest_one(s, idx)) {
            return;
        }
        s->pend_n--;
        for (int i = 0; i < s->pend_n; i++) {
            s->pend_q[i] = s->pend_q[i + 1];
        }
    }
}

/* ── screencopy 帧事件 ──────────────────────────────────────── */
static void fr_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt,
                      uint32_t w, uint32_t h, uint32_t stride)
{
    (void)d; (void)f; (void)fmt; (void)w; (void)h; (void)stride;
}
static void fr_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t flags)
{
    (void)d; (void)f; (void)flags;
}
static void fr_damage(void *d, struct zwlr_screencopy_frame_v1 *f,
                      uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    (void)d; (void)f; (void)x; (void)y; (void)w; (void)h;
}

static void fr_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *f,
                            uint32_t fmt, uint32_t w, uint32_t h)
{
    (void)f;
    struct sc_req *r = data;
    struct ww_sc *s = r->s;
    if (fmt != DRM_FORMAT_ARGB8888 && fmt != DRM_FORMAT_XRGB8888) {
        if (!s->fail_logged) {
            s->fail_logged = 1;
            fprintf(stderr, "[sc] 不支持的 dmabuf 格式 0x%x（只支持 ARGB8888/XRGB8888）\n", fmt);
        }
        return;
    }
    slot_build(s, r->slot, fmt, w, h);
}

static void fr_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *f)
{
    struct sc_req *r = data;
    struct ww_sc *s = r->s;
    if (!s->slots[r->slot].wl_buf) {
        if (!s->fail_logged) {
            s->fail_logged = 1;
            fprintf(stderr, "[sc] buffer_done 时没有可用 wl_buffer\n");
        }
        zwlr_screencopy_frame_v1_destroy(f);
        s->reqs[r->slot] = NULL;
        if (s->inflight > 0) {
            s->inflight--;
        }
        s->slots[r->slot].state = SLOT_FREE;
        free(r);
        return;
    }
    zwlr_screencopy_frame_v1_copy(f, s->slots[r->slot].wl_buf);
}

static void issue_next(struct ww_sc *s);

static void fr_ready(void *data, struct zwlr_screencopy_frame_v1 *f,
                     uint32_t sec_hi, uint32_t sec_lo, uint32_t nsec)
{
    struct sc_req *r = data;
    struct ww_sc *s = r->s;
    int idx = r->slot;
    zwlr_screencopy_frame_v1_destroy(f);
    s->reqs[idx] = NULL;
    if (s->inflight > 0) {
        s->inflight--;
    }
    free(r);

    struct sc_slot *sl = &s->slots[idx];
    uint64_t t_ready = now_us();
    if (sl->req_us && t_ready > sl->req_us) {
        s->sum_cap_us += t_ready - sl->req_us;
        s->n_cap++;
    }
    sl->state = SLOT_CAPTURED;
    s->ready = true;

    uint64_t pts = (((uint64_t)sec_hi << 32) | sec_lo) * 1000000ull + nsec / 1000ull;
    sl->pts_us = pts;

    /* 先把当前帧提交给 GPU 转换（异步），再去取上一帧的回读。
     * 这样每帧只等一次 fence，且等的是已经跑完一帧的旧 fence，几乎不阻塞。 */
    if (convert_slot(s, idx) == 0 && s->pend_n < WW_SC_SLOTS) {
        s->pend_q[s->pend_n++] = idx;
    } else {
        sl->state = SLOT_FREE;
    }
    drain_pending(s);

    /* 回读已经把旧槽位释放出来了，这时发下一帧才拿得到槽位 */
    issue_next(s);
}

static void fr_failed(void *data, struct zwlr_screencopy_frame_v1 *f)
{
    struct sc_req *r = data;
    struct ww_sc *s = r->s;
    int idx = r->slot;
    zwlr_screencopy_frame_v1_destroy(f);
    s->reqs[idx] = NULL;
    if (s->inflight > 0) {
        s->inflight--;
    }
    free(r);
    s->slots[idx].state = SLOT_FREE;
    if (!s->fail_logged) {
        s->fail_logged = 1;
        fprintf(stderr, "[sc] screencopy 失败（dmabuf 拷贝被拒绝）\n");
    }
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = fr_buffer,
    .flags = fr_flags,
    .ready = fr_ready,
    .failed = fr_failed,
    .damage = fr_damage,
    .linux_dmabuf = fr_linux_dmabuf,
    .buffer_done = fr_buffer_done,
};

static void issue_next(struct ww_sc *s)
{
    if (!s->mgr || s->inflight >= WW_SC_SLOTS) {
        return;
    }
    int idx = -1;
    for (int k = 0; k < WW_SC_SLOTS; k++) {
        int c = (s->next_slot + k) % WW_SC_SLOTS;
        if (s->slots[c].state == SLOT_FREE) {
            idx = c;
            break;
        }
    }
    if (idx < 0) {
        return;
    }
    s->next_slot = (idx + 1) % WW_SC_SLOTS;

    struct zwlr_screencopy_frame_v1 *f =
        zwlr_screencopy_manager_v1_capture_output(s->mgr, 0, s->output);
    if (!f) {
        return;
    }
    struct sc_req *r = calloc(1, sizeof(*r));
    if (!r) {
        zwlr_screencopy_frame_v1_destroy(f);
        return;
    }
    r->s = s;
    r->slot = idx;
    r->req_us = now_us();
    r->frame = f;

    s->slots[idx].state = SLOT_PENDING;
    s->slots[idx].req_us = r->req_us;
    s->reqs[idx] = r;
    s->inflight++;
    s->last_req_us = r->req_us;
    zwlr_screencopy_frame_v1_add_listener(f, &frame_listener, r);
    wl_display_flush(s->dpy);
}

void ww_sc_tick(struct ww_sc *s)
{
    if (!s) {
        return;
    }
    /* 每轮循环都试着取回已完成的回读。compositor 每个刷新周期只服务一个
     * screencopy，槽位回收得越及时，下一个请求就越早排上队，吞吐才上得去。 */
    drain_pending(s);

    if (s->inflight >= WW_SC_SLOTS) {
        return;
    }
    uint64_t now = now_us();
    /* 半帧间隔发请求：compositor 每个刷新周期只服务一个 screencopy，
     * 队列里多压一个请求才能保证每个刷新点都有活干，否则会漏刷新、掉到 50 出头。 */
    if (s->last_req_us && now - s->last_req_us < s->period_us) {
        return;
    }
    issue_next(s);
}

bool ww_sc_ready(struct ww_sc *s)
{
    return s && s->ready;
}

void ww_sc_set_fps(struct ww_sc *s, int fps)
{
    if (!s || fps <= 0) {
        return;
    }
    s->fps = fps;
    s->period_us = (uint64_t)(1000000 / fps);
}

double ww_sc_take_cap_ms(struct ww_sc *s)
{
    if (!s) {
        return 0.0;
    }
    double ms = s->n_cap ? (double)s->sum_cap_us / (double)s->n_cap / 1000.0 : 0.0;
    s->sum_cap_us = 0;
    s->n_cap = 0;
    return ms;
}

/* ── DRM 节点归属判定 ─────────────────────────────────────────
 * EGL 上下文必须和抓帧的 gbm 设备在同一张卡上：跨卡导入 tiled dmabuf 会直接
 * EGL_BAD_PARAMETER（实测：i915 的 Y-tiled 缓冲导入 NVIDIA 的 EGL 上下文即失败）。
 * cardN 与 renderDNNN 是两个节点，但在 sysfs 里指向同一个 PCI device，
 * 所以用 /sys/class/drm/<node>/device 的 realpath 比较，避免混淆两种节点名。 */
static bool node_same_gpu(const char *a, const char *b)
{
    if (!a || !b || !*a || !*b) {
        return false;
    }
    char pa[PATH_MAX], pb[PATH_MAX], ra[PATH_MAX], rb[PATH_MAX];
    const char *ba = strrchr(a, '/');
    const char *bb = strrchr(b, '/');
    ba = ba ? ba + 1 : a;
    bb = bb ? bb + 1 : b;
    snprintf(pa, sizeof(pa), "/sys/class/drm/%s/device", ba);
    snprintf(pb, sizeof(pb), "/sys/class/drm/%s/device", bb);
    if (!realpath(pa, ra) || !realpath(pb, rb)) {
        return false;
    }
    return strcmp(ra, rb) == 0;
}

/* 该 DRM 节点是否是 NVIDIA 卡（vendor 0x10de） */
static bool node_is_nvidia(const char *node)
{
    if (!node || !*node) {
        return false;
    }
    char p[PATH_MAX], buf[64] = {0};
    const char *b = strrchr(node, '/');
    b = b ? b + 1 : node;
    snprintf(p, sizeof(p), "/sys/class/drm/%s/device/vendor", b);
    FILE *f = fopen(p, "r");
    if (!f) {
        return false;
    }
    bool ok = fgets(buf, sizeof(buf), f) && strstr(buf, "0x10de");
    fclose(f);
    return ok;
}

/* EGL 设备对应的 DRM 节点（优先 render node，其次 primary node） */
static const char *egl_device_node(EGLDeviceEXT dev, char *out, size_t n)
{
    /* 扩展入口不在 libEGL 的导出符号里，必须走 eglGetProcAddress */
    PFNEGLQUERYDEVICESTRINGEXTPROC qs =
        (PFNEGLQUERYDEVICESTRINGEXTPROC)eglGetProcAddress("eglQueryDeviceStringEXT");
    if (!qs) {
        return NULL;
    }
    const char *exts = qs(dev, EGL_EXTENSIONS);
    if (!exts) {
        return NULL;
    }
#ifdef EGL_DRM_RENDER_NODE_FILE_EXT
    if (strstr(exts, "EGL_EXT_device_drm_render_node")) {
        const char *p = qs(dev, EGL_DRM_RENDER_NODE_FILE_EXT);
        if (p && *p) {
            snprintf(out, n, "%s", p);
            return out;
        }
    }
#endif
    if (strstr(exts, "EGL_EXT_device_drm")) {
        const char *p = qs(dev, EGL_DRM_DEVICE_FILE_EXT);
        if (p && *p) {
            snprintf(out, n, "%s", p);
            return out;
        }
    }
    return NULL;
}

/* ── EGL / GL 初始化 ───────────────────────────────────────── */
static int init_egl_gl(struct ww_sc *s, const char *want_node)
{
    PFNEGLQUERYDEVICESEXTPROC query_devices =
        (PFNEGLQUERYDEVICESEXTPROC)eglGetProcAddress("eglQueryDevicesEXT");
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    PFNEGLBINDAPIPROC bind_api =
        (PFNEGLBINDAPIPROC)eglGetProcAddress("eglBindAPI");
    s->gl_image_target =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    s->egl_create_image =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    s->egl_destroy_image =
        (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    s->gl_gen_fbos = (PFNGLGENFRAMEBUFFERSPROC)eglGetProcAddress("glGenFramebuffers");
    s->gl_del_fbos = (PFNGLDELETEFRAMEBUFFERSPROC)eglGetProcAddress("glDeleteFramebuffers");
    s->gl_bind_fbo = (PFNGLBINDFRAMEBUFFERPROC)eglGetProcAddress("glBindFramebuffer");
    s->gl_fbo_tex = (PFNGLFRAMEBUFFERTEXTURE2DPROC)eglGetProcAddress("glFramebufferTexture2D");
    s->gl_fence_sync = (PFNGLFENCESYNCPROC)eglGetProcAddress("glFenceSync");
    s->gl_client_wait_sync =
        (PFNGLCLIENTWAITSYNCPROC)eglGetProcAddress("glClientWaitSync");
    s->gl_delete_sync = (PFNGLDELETESYNCPROC)eglGetProcAddress("glDeleteSync");
    s->gl_map_buffer_range =
        (PFNGLMAPBUFFERRANGEPROC)eglGetProcAddress("glMapBufferRange");
    s->gl_unmap_buffer = (PFNGLUNMAPBUFFERPROC)eglGetProcAddress("glUnmapBuffer");
    if (!query_devices || !get_platform_display || !bind_api || !s->gl_image_target ||
        !s->egl_create_image || !s->egl_destroy_image || !s->gl_gen_fbos ||
        !s->gl_del_fbos || !s->gl_bind_fbo || !s->gl_fbo_tex ||
        !s->gl_fence_sync || !s->gl_client_wait_sync || !s->gl_delete_sync ||
        !s->gl_map_buffer_range || !s->gl_unmap_buffer) {
        fprintf(stderr, "[sc] EGL/GL 扩展缺失\n");
        return -1;
    }

    EGLDeviceEXT devs[8];
    EGLint nd = 0;
    if (!query_devices(8, devs, &nd) || nd <= 0) {
        fprintf(stderr, "[sc] eglQueryDevicesEXT 失败\n");
        return -1;
    }
    /* 先按 DRM 节点定位要用的设备：抓帧缓冲来自整个 gbm 卡，
       GL 必须落在同一张卡上，否则 dmabuf 导入必失败。 */
    EGLint want_idx = -1;
    if (want_node && *want_node) {
        for (EGLint i = 0; i < nd; i++) {
            char nb[PATH_MAX];
            const char *n = egl_device_node(devs[i], nb, sizeof(nb));
            if (n && node_same_gpu(n, want_node)) {
                want_idx = i;
                break;
            }
        }
        if (want_idx >= 0) {
            fprintf(stderr, "[sc] EGL 设备 %d 与抓帧卡 %s 同属一张 GPU\n", want_idx, want_node);
        } else {
            /* 不匹配就退回旧行为，但明确告警：跨卡导入 tiled dmabuf 会失败 */
            fprintf(stderr, "[sc] 没有 EGL 设备对应 %s（跨卡导入大概率失败）\n", want_node);
        }
    }

    EGLDisplay inited[8];
    EGLint inited_idx[8];
    EGLint ninit = 0;
    for (EGLint i = 0; i < nd && ninit < 8; i++) {
        EGLDisplay d = get_platform_display(EGL_PLATFORM_DEVICE_EXT, devs[i], NULL);
        EGLint major = 0, minor = 0;
        if (d == EGL_NO_DISPLAY || !eglInitialize(d, &major, &minor)) {
            continue;
        }
        const char *vendor = eglQueryString(d, EGL_VENDOR);
        fprintf(stderr, "[sc] EGL[%d] %s\n", i, vendor ? vendor : "-");
        inited[ninit] = d;
        inited_idx[ninit] = i;
        ninit++;
        if (i == want_idx) {
            break;   /* 命中目标设备，其余不必再初始化 */
        }
    }

    /* 选定：节点匹配优先；否则沿用「NVIDIA 优先」的旧策略，再否则取第一个 */
    if (want_idx >= 0) {
        for (EGLint k = 0; k < ninit; k++) {
            if (inited_idx[k] == want_idx) {
                s->edpy = inited[k];
                break;
            }
        }
    }
    if (s->edpy == EGL_NO_DISPLAY) {
        for (EGLint k = 0; k < ninit; k++) {
            const char *v = eglQueryString(inited[k], EGL_VENDOR);
            if (v && strstr(v, "NVIDIA")) {
                s->edpy = inited[k];
                break;
            }
        }
    }
    if (s->edpy == EGL_NO_DISPLAY && ninit > 0) {
        s->edpy = inited[0];
    }
    for (EGLint k = 0; k < ninit; k++) {
        if (inited[k] != s->edpy) {
            eglTerminate(inited[k]);
        }
    }
    if (s->edpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "[sc] 没有可用的 EGL display\n");
        return -1;
    }

    if (!bind_api(EGL_OPENGL_API)) {
        fprintf(stderr, "[sc] eglBindAPI 失败\n");
        return -1;
    }
    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_NONE
    };
    EGLConfig cfg;
    EGLint ncfg = 0;
    eglChooseConfig(s->edpy, cfg_attr, &cfg, 1, &ncfg);
    s->ectx = eglCreateContext(s->edpy, ncfg ? cfg : NULL, EGL_NO_CONTEXT, NULL);
    if (!s->ectx) {
        fprintf(stderr, "[sc] eglCreateContext 失败 0x%x\n", eglGetError());
        return -1;
    }
    if (!eglMakeCurrent(s->edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, s->ectx)) {
        fprintf(stderr, "[sc] eglMakeCurrent 失败 0x%x\n", eglGetError());
        return -1;
    }
    fprintf(stderr, "[sc] GL 就绪: %s\n", (const char *)glGetString(GL_RENDERER));

    s->prog_y = compile_prog(FS_Y_SRC, "Y");
    s->prog_uv = compile_prog(FS_UV_SRC, "UV");
    if (!s->prog_y || !s->prog_uv) {
        return -1;
    }
    s->u_y_src = glGetUniformLocation(s->prog_y, "u_src");
    s->u_uv_src = glGetUniformLocation(s->prog_uv, "u_src");
    return 0;
}

struct ww_sc *ww_sc_create(struct wl_display *dpy, struct wl_output *output,
                           const char *gbm_node, int fps, ww_sc_frame_cb cb, void *ud)
{
    struct ww_sc *s = calloc(1, sizeof(*s));
    if (!s) {
        return NULL;
    }
    s->dpy = dpy;
    s->output = output;
    s->cb = cb;
    s->ud = ud;
    for (int i = 0; i < WW_SC_SLOTS; i++) {
        s->slots[i].fd = -1;
        s->slots[i].img = EGL_NO_IMAGE_KHR;
    }
    ww_sc_set_fps(s, fps > 0 ? fps : 60);

    /* 强制厂商库只对 NVIDIA 抓帧有意义：glvnd 被锁成 nvidia-only 后，
       Mesa 的 i915/iris 设备根本不会被枚举到，集显路径永远建不出同卡上下文。 */
    if (!getenv("__EGL_VENDOR_LIBRARY_FILENAMES") && node_is_nvidia(gbm_node)) {
        setenv("__EGL_VENDOR_LIBRARY_FILENAMES",
               "/usr/share/glvnd/egl_vendor.d/10_nvidia.json", 1);
    }

    if (init_egl_gl(s, gbm_node) != 0) {
        goto fail;
    }

    /* registry：screencopy + linux-dmabuf */
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &reg_listener, s);
    wl_display_roundtrip(dpy);
    if (!s->mgr || !s->dmabuf) {
        fprintf(stderr, "[sc] 缺少 wlr-screencopy / linux-dmabuf（mgr=%p dmabuf=%p）\n",
                (void *)s->mgr, (void *)s->dmabuf);
        goto fail;
    }

    int fd = open(gbm_node, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "[sc] 打开 %s 失败: %s\n", gbm_node, strerror(errno));
        goto fail;
    }
    s->gbm = gbm_create_device(fd);
    if (!s->gbm) {
        close(fd);
        fprintf(stderr, "[sc] gbm_create_device(%s) 失败\n", gbm_node);
        goto fail;
    }
    s->gbm_fd = fd;
    fprintf(stderr, "[sc] gbm 后端 %s\n", gbm_device_get_backend_name(s->gbm));

    fprintf(stderr, "[sc] 零拷贝抓帧就绪（%s）\n", gbm_node);
    return s;

fail:
    ww_sc_destroy(s);
    return NULL;
}

void ww_sc_destroy(struct ww_sc *s)
{
    if (!s) {
        return;
    }
    for (int i = 0; i < WW_SC_SLOTS; i++) {
        if (s->reqs[i]) {
            if (s->reqs[i]->frame) {
                zwlr_screencopy_frame_v1_destroy(s->reqs[i]->frame);
            }
            free(s->reqs[i]);
            s->reqs[i] = NULL;
        }
    }
    for (int i = 0; i < WW_SC_SLOTS; i++) {
        slot_teardown(s, i);
    }
    free_gpu_buffers(s);

    if (s->prog_y) {
        glDeleteProgram(s->prog_y);
        s->prog_y = 0;
    }
    if (s->prog_uv) {
        glDeleteProgram(s->prog_uv);
        s->prog_uv = 0;
    }
    if (s->ectx) {
        eglMakeCurrent(s->edpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroyContext(s->edpy, s->ectx);
        s->ectx = NULL;
    }
    if (s->edpy != EGL_NO_DISPLAY) {
        eglTerminate(s->edpy);
        s->edpy = EGL_NO_DISPLAY;
    }
    if (s->gbm) {
        gbm_device_destroy(s->gbm);
        s->gbm = NULL;
    }
    if (s->gbm_fd >= 0) {
        close(s->gbm_fd);
        s->gbm_fd = -1;
    }
    free(s);
}
