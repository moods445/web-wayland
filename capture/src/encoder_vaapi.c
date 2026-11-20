/*
 * VAAPI 编码器：dmabuf(ARGB) --VPP--> NV12 --h264_vaapi(VDEnc/CQP)--> Annex-B
 *
 * 关键点（均为本机实测得出）：
 *  - HD 630 的 H.264 只有 VAEntrypointEncSliceLP 入口，故必须 low_power=1
 *  - 该入口只支持 CQP 码控，没有 CBR/VBR，因此自适应策略调 QP
 *  - dmabuf 与 compositor 同卡（renderD129），VPP 与编码共用同一个 VADisplay，全程 GPU
 */

#include "encoder.h"

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define WW_SWS_THREADS 4

#ifdef HAVE_LIBYUV
/* SIMD 加速的 BT.709 色彩转换（见 yuv_shim.cc）；失败时回退 sws */
extern int ww_argb_to_nv12(const unsigned char *src, int src_stride,
                           unsigned char *dst_y, int dst_stride_y,
                           unsigned char *dst_uv, int dst_stride_uv,
                           int width, int height);
#endif

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vaapi.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libswscale/swscale.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <va/va_vpp.h>

struct ww_enc {
    AVBufferRef *hwdev;
    AVBufferRef *frames;
    AVCodecContext *ctx;
    VADisplay dpy;
    int w, h, fps, qp;
    VAConfigID vpp_cfg;
    VAContextID vpp_ctx;
    bool vpp_ready;
    bool use_sws;          /* VPP 不可用时回退 CPU 色彩转换 */
    struct SwsContext *sws;            /* 单线程路径 */
    struct SwsContext *sws_mt[WW_SWS_THREADS];  /* 多线程分带路径 */
    AVFrame *sw_frame;
    AVFrame *nv12_frame;   /* 零拷贝抓帧路径的 NV12 承载帧（复用，避免每帧分配） */
    bool needs_idr;
    int kind;              /* 0 = vaapi（集显 VDEnc）, 1 = nvenc（独显） */
    bool chroma444;        /* true = yuv444p（仅 NVENC） */
    int64_t bitrate_bps;   /* >0 = 目标码率；实际是否走 CBR 看 cbr */
    bool cbr;              /* true = 已按 CBR 打开，qp 不参与码控 */
    struct SwsContext *sws_nv12_444;   /* 零拷贝 NV12 → YUV444P 回退转换 */
    /* 内部分段耗时（微秒） */
    uint64_t stat_conv_us, stat_up_us, stat_send_us, stat_n;
};

static uint64_t enc_now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ull + (uint64_t)ts.tv_nsec / 1000ull;
}

/* 给上层打印用：取走并清零累计值 */
void ww_enc_take_stats(struct ww_enc *e, double *conv_ms, double *up_ms, double *send_ms)
{
    double n = e->stat_n ? (double)e->stat_n : 1.0;
    *conv_ms = (double)e->stat_conv_us / n / 1000.0;
    *up_ms = (double)e->stat_up_us / n / 1000.0;
    *send_ms = (double)e->stat_send_us / n / 1000.0;
    e->stat_conv_us = e->stat_up_us = e->stat_send_us = e->stat_n = 0;
}

/* 分带转换的线程参数 */
struct sws_band {
    struct SwsContext *ctx;
    const uint8_t *src;
    int src_stride;
    uint8_t *dst[4];
    int dst_stride[4];
    int y0;
    int rows;
};

static void *sws_band_run(void *arg)
{
    struct sws_band *b = arg;
    const uint8_t *s[4] = { b->src, NULL, NULL, NULL };
    int ss[4] = { b->src_stride, 0, 0, 0 };
    sws_scale(b->ctx, s, ss, b->y0, b->rows, b->dst, b->dst_stride);
    return NULL;
}

static VASurfaceID import_dmabuf(struct ww_enc *e, int fd, uint32_t fourcc, uint32_t w, uint32_t h,
                                 uint32_t stride, uint64_t modifier)
{
    VADRMPRIMESurfaceDescriptor desc;
    memset(&desc, 0, sizeof(desc));

    desc.fourcc = fourcc;
    desc.width = w;
    desc.height = h;
    desc.num_objects = 1;
    desc.objects[0].fd = fd;
    desc.objects[0].size = (uint64_t)stride * h;
    desc.objects[0].drm_format_modifier = modifier;
    desc.num_layers = 1;
    desc.layers[0].drm_format = fourcc;
    desc.layers[0].num_planes = 1;
    desc.layers[0].object_index[0] = 0;
    desc.layers[0].offset[0] = 0;
    desc.layers[0].pitch[0] = stride;

    VASurfaceAttrib attrs[2];
    memset(attrs, 0, sizeof(attrs));
    attrs[0].type = VASurfaceAttribMemoryType;
    attrs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[0].value.type = VAGenericValueTypeInteger;
    attrs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;

    attrs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attrs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attrs[1].value.type = VAGenericValueTypePointer;
    attrs[1].value.value.p = &desc;

    VASurfaceID surf = VA_INVALID_SURFACE;
    VAStatus st = vaCreateSurfaces(e->dpy, VA_RT_FORMAT_RGB32, w, h, &surf, 1, attrs, 2);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "[enc] vaCreateSurfaces(import) 失败: %s\n", vaErrorStr(st));
        return VA_INVALID_SURFACE;
    }
    return surf;
}

static int vpp_init(struct ww_enc *e)
{
    VAStatus st = vaCreateConfig(e->dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &e->vpp_cfg);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "[enc] vaCreateConfig(VPP) 失败: %s\n", vaErrorStr(st));
        return -1;
    }
    st = vaCreateContext(e->dpy, e->vpp_cfg, e->w, e->h, 0, NULL, 0, &e->vpp_ctx);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "[enc] vaCreateContext(VPP) 失败: %s\n", vaErrorStr(st));
        return -1;
    }
    e->vpp_ready = true;
    return 0;
}

static int vpp_convert(struct ww_enc *e, VASurfaceID src, VASurfaceID dst)
{
    VABufferID buf = VA_INVALID_ID;
    VAStatus st = vaCreateBuffer(e->dpy, e->vpp_ctx, VAProcPipelineParameterBufferType,
                                 sizeof(VAProcPipelineParameterBuffer), 1, NULL, &buf);
    if (st != VA_STATUS_SUCCESS) {
        return -1;
    }

    VAProcPipelineParameterBuffer *pipe = NULL;
    st = vaMapBuffer(e->dpy, buf, (void **)&pipe);
    if (st != VA_STATUS_SUCCESS) {
        vaDestroyBuffer(e->dpy, buf);
        return -1;
    }
    memset(pipe, 0, sizeof(*pipe));
    pipe->surface = src;
    pipe->output_background_color = 0xff000000;
    vaUnmapBuffer(e->dpy, buf);

    st = vaBeginPicture(e->dpy, e->vpp_ctx, dst);
    if (st == VA_STATUS_SUCCESS) {
        st = vaRenderPicture(e->dpy, e->vpp_ctx, &buf, 1);
    }
    if (st == VA_STATUS_SUCCESS) {
        st = vaEndPicture(e->dpy, e->vpp_ctx);
    }
    vaDestroyBuffer(e->dpy, buf);
    if (st != VA_STATUS_SUCCESS) {
        fprintf(stderr, "[enc] VPP 转换失败: %s\n", vaErrorStr(st));
        return -1;
    }
    return 0;
}

/* ── 码控能力：CBR（目标码率）不是所有入口都支持 ──
   实测 HD 630 的 VDEnc（VAEntrypointEncSliceLP）只有 VA_RC_CQP，
   ffmpeg 会直接报 "Driver does not support CBR RC mode"。 */
static unsigned vaapi_rc_mask(VADisplay dpy)
{
    static const VAEntrypoint eps[] = { VAEntrypointEncSliceLP, VAEntrypointEncSlice };
    unsigned mask = 0;
    for (size_t i = 0; i < sizeof(eps) / sizeof(eps[0]); i++) {
        VAConfigAttrib a;
        memset(&a, 0, sizeof(a));
        a.type = VAConfigAttribRateControl;
        if (vaGetConfigAttributes(dpy, VAProfileH264High, eps[i], &a, 1) == VA_STATUS_SUCCESS
            && a.value != VA_ATTRIB_NOT_SUPPORTED) {
            /* 返回的是受支持码控模式的位掩码（见 VAAPI 规范 VAConfigAttribRateControl） */
            mask |= a.value;
        }
    }
    return mask;
}

/* ── 独显后端：h264_nvenc（软件 NV12 帧直接送入，NVENC 自行上传）── */
static struct ww_enc *create_nvenc(struct ww_enc *e, int w, int h, int fps, int qp, bool chroma444,
                                  int64_t bitrate_bps)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) {
        fprintf(stderr, "[enc] 没有 h264_nvenc（驱动或 ffmpeg 不支持）\n");
        return NULL;
    }
    e->kind = 1;
    e->ctx = avcodec_alloc_context3(codec);
    if (!e->ctx) {
        fprintf(stderr, "[enc] 分配 nvenc 上下文失败\n");
        return NULL;
    }
    e->ctx->width = w;
    e->ctx->height = h;
    e->ctx->time_base = (AVRational){1, 1000000};
    e->ctx->framerate = (AVRational){fps, 1};
    e->chroma444 = chroma444;
    e->ctx->pix_fmt = chroma444 ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_NV12;
    e->ctx->profile = chroma444 ? FF_PROFILE_H264_HIGH_444_PREDICTIVE : FF_PROFILE_H264_HIGH;
    e->ctx->max_b_frames = 0;
    e->ctx->gop_size = 120;
    /* NVENC 用 priv 的 qp（constqp），不要设 global_quality，否则会告警 */
    e->ctx->colorspace = AVCOL_SPC_SMPTE170M;
    e->ctx->color_primaries = AVCOL_PRI_SMPTE170M;
    e->ctx->color_trc = AVCOL_TRC_SMPTE170M;
    e->ctx->color_range = AVCOL_RANGE_MPEG;

    /* 低延迟组合：p4 + ull + 不排队；码控二选一：CBR（目标码率）或 constqp */
    av_opt_set(e->ctx->priv_data, "preset", "p4", 0);
    av_opt_set(e->ctx->priv_data, "tune", "ull", 0);
    if (bitrate_bps > 0) {
        /* NVENC 全系支持 CBR：静态画面也会用填充把瞬时码率顶到目标值 */
        av_opt_set(e->ctx->priv_data, "rc", "cbr", 0);
        av_opt_set_int(e->ctx->priv_data, "bitrate", bitrate_bps, 0);
        av_opt_set_int(e->ctx->priv_data, "maxrate", bitrate_bps, 0);
        av_opt_set_int(e->ctx->priv_data, "bufsize", bitrate_bps, 0);
        e->ctx->bit_rate = bitrate_bps;
        e->ctx->rc_max_rate = bitrate_bps;
        e->ctx->rc_buffer_size = (int)bitrate_bps;
        e->cbr = true;
    } else {
        av_opt_set(e->ctx->priv_data, "rc", "constqp", 0);
        av_opt_set_int(e->ctx->priv_data, "qp", qp, 0);
    }
    av_opt_set_int(e->ctx->priv_data, "delay", 0, 0);
    av_opt_set_int(e->ctx->priv_data, "zerolatency", 1, 0);

    if (avcodec_open2(e->ctx, codec, NULL) < 0) {
        fprintf(stderr, "[enc] h264_nvenc 打开失败（QP=%d，目标码率 %lld bps）\n",
                qp, (long long)bitrate_bps);
        /* 不释放会让调用方每帧重建时都泄漏一个 AVCodecContext（含 GPU 资源） */
        avcodec_free_context(&e->ctx);
        return NULL;
    }
    if (e->cbr) {
        fprintf(stderr, "[enc] h264_nvenc 就绪 %dx%d@%d 色度=%s 目标码率 %.1f Mbps (cbr/ull)\n",
                w, h, fps, chroma444 ? "4:4:4" : "4:2:0", (double)bitrate_bps / 1e6);
    } else {
        fprintf(stderr, "[enc] h264_nvenc 就绪 %dx%d@%d qp=%d 色度=%s (constqp/ull)\n",
                w, h, fps, qp, chroma444 ? "4:4:4" : "4:2:0");
    }
    return e;
}

struct ww_enc *ww_enc_create_full(const char *kind, const char *drm_node,
                                  int w, int h, int fps, int qp, bool chroma444,
                                  int64_t bitrate_bps)
{
    struct ww_enc *e = calloc(1, sizeof(*e));
    if (!e) {
        return NULL;
    }
    e->w = w;
    e->h = h;
    e->fps = fps;
    e->qp = qp;
    e->bitrate_bps = bitrate_bps > 0 ? bitrate_bps : 0;

    if (kind && !strcmp(kind, "nvenc")) {
        if (!create_nvenc(e, w, h, fps, qp, chroma444, e->bitrate_bps)) {
            free(e);
            return NULL;
        }
        return e;
    }
    if (chroma444) {
        /* HD 630 的 VDEnc 只支持 4:2:0，调用方应先探测再请求 */
        fprintf(stderr, "[enc] VAAPI 不支持 H.264 4:4:4\n");
        free(e);
        return NULL;
    }
    if (av_hwdevice_ctx_create(&e->hwdev, AV_HWDEVICE_TYPE_VAAPI, drm_node, NULL, 0) < 0) {
        fprintf(stderr, "[enc] 无法创建 VAAPI 设备 %s\n", drm_node);
        free(e);
        return NULL;
    }
    /* hwdev->data 是 AVHWDeviceContext，其 ->hwctx 才是 VAAPI 后端上下文 */
    AVHWDeviceContext *hwctx = (AVHWDeviceContext *)e->hwdev->data;
    AVVAAPIDeviceContext *dc = (AVVAAPIDeviceContext *)hwctx->hwctx;
    e->dpy = dc->display;

    e->frames = av_hwframe_ctx_alloc(e->hwdev);
    if (!e->frames) {
        goto fail;
    }
    AVHWFramesContext *fc = (AVHWFramesContext *)e->frames->data;
    fc->format = AV_PIX_FMT_VAAPI;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = w;
    fc->height = h;
    fc->initial_pool_size = 16;
    if (av_hwframe_ctx_init(e->frames) < 0) {
        fprintf(stderr, "[enc] hwframe_ctx_init 失败\n");
        goto fail;
    }

    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!codec) {
        fprintf(stderr, "[enc] 找不到 h264_vaapi\n");
        goto fail;
    }
    e->ctx = avcodec_alloc_context3(codec);
    if (!e->ctx) {
        fprintf(stderr, "[enc] 分配 vaapi 上下文失败\n");
        goto fail;
    }
    e->ctx->width = w;
    e->ctx->height = h;
    e->ctx->time_base = (AVRational){1, 1000000};
    e->ctx->framerate = (AVRational){fps, 1};
    e->ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    e->ctx->profile = FF_PROFILE_H264_HIGH;
    e->ctx->max_b_frames = 0;
    e->ctx->gop_size = 120;
    e->ctx->hw_frames_ctx = av_buffer_ref(e->frames);
    e->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    /* 目标码率只有在驱动支持 CBR 时才有意义（HD 630 的 VDEnc 实测只有 CQP） */
    if (e->bitrate_bps > 0 && (vaapi_rc_mask(e->dpy) & VA_RC_CBR)) {
        e->ctx->bit_rate = e->bitrate_bps;
        e->ctx->rc_max_rate = e->bitrate_bps;
        e->ctx->rc_buffer_size = (int)e->bitrate_bps;
        if (av_opt_set(e->ctx->priv_data, "rc_mode", "CBR", 0) == 0) {
            e->cbr = true;
        }
    }
    if (!e->cbr) {
        if (e->bitrate_bps > 0) {
            fprintf(stderr, "[enc] 驱动不支持 CBR，目标码率 %.1f Mbps 忽略（仅 CQP）\n",
                    (double)e->bitrate_bps / 1e6);
        }
        e->bitrate_bps = 0;
        /* 必须同时打上 QSCALE 标志，否则 global_quality 会被忽略（QP 永远停在驱动默认值） */
        e->ctx->flags |= AV_CODEC_FLAG_QSCALE;
        e->ctx->global_quality = qp * FF_QP2LAMBDA;
        if (av_opt_set(e->ctx->priv_data, "rc_mode", "CQP", 0) < 0) {
            fprintf(stderr, "[enc] 设置 rc_mode=CQP 失败\n");
        }
    }
    av_opt_set_int(e->ctx->priv_data, "low_power", 1, 0);
    /* async_depth 决定编码器里"在途"的帧数：每多存一帧就多一帧显示延迟。
       8 帧在 60fps 下等于 ~130ms，鼠标会明显拖尾；1 表示提交后立刻取包，
       延迟只剩编码本身。1080p VDEnc 单帧编码远小于 16.6ms，吞吐不受影响。 */
    av_opt_set_int(e->ctx->priv_data, "async_depth", 1, 0);

    /* 信令必须与 libyuv 的 ARGBToI420 一致（BT.601 limited），否则解码端按 709 解释会发灰 */
    e->ctx->colorspace = AVCOL_SPC_SMPTE170M;
    e->ctx->color_primaries = AVCOL_PRI_SMPTE170M;
    e->ctx->color_trc = AVCOL_TRC_SMPTE170M;
    e->ctx->color_range = AVCOL_RANGE_MPEG;

    if (avcodec_open2(e->ctx, codec, NULL) < 0) {
        fprintf(stderr, "[enc] avcodec_open2 失败（QP=%d，目标码率 %lld bps）\n",
                qp, (long long)e->bitrate_bps);
        goto fail;
    }

    if (vpp_init(e) != 0) {
        e->use_sws = true;
        fprintf(stderr, "[enc] 驱动未提供 VPP，回退 CPU 色彩转换（ARGB→NV12）\n");
    }

    if (e->cbr) {
        fprintf(stderr, "[enc] h264_vaapi 就绪 %dx%d@%d 目标码率 %.1f Mbps (VDEnc/CBR)\n",
                w, h, fps, (double)e->bitrate_bps / 1e6);
    } else {
        fprintf(stderr, "[enc] h264_vaapi 就绪 %dx%d@%d qp=%d (VDEnc/CQP)\n", w, h, fps, qp);
    }
    return e;

fail:
    /* 创建失败必须释放已分配的对象：调用方会每帧重试，
       漏掉这里会在几十秒内耗尽 DRM fd / 显存，随后 wwcap 永久黑屏 */
    if (e->ctx) {
        avcodec_free_context(&e->ctx);
    }
    if (e->frames) {
        av_buffer_unref(&e->frames);
    }
    if (e->hwdev) {
        av_buffer_unref(&e->hwdev);
    }
    free(e);
    return NULL;
}

struct ww_enc *ww_enc_create_chroma(const char *kind, const char *drm_node,
                                    int w, int h, int fps, int qp, bool chroma444)
{
    return ww_enc_create_full(kind, drm_node, w, h, fps, qp, chroma444, 0);
}

struct ww_enc *ww_enc_create_ex(const char *kind, const char *drm_node,
                                int w, int h, int fps, int qp)
{
    return ww_enc_create_chroma(kind, drm_node, w, h, fps, qp, false);
}

struct ww_enc *ww_enc_create(const char *drm_node, int w, int h, int fps, int qp)
{
    return ww_enc_create_ex("vaapi", drm_node, w, h, fps, qp);
}

/* 探测目标码率（CBR）能力：NVENC 全系支持；VAAPI 取决于驱动的码控属性 */
bool ww_enc_probe_cbr(const char *kind, const char *drm_node)
{
    if (kind && !strcmp(kind, "nvenc")) {
        return avcodec_find_encoder_by_name("h264_nvenc") != NULL;
    }
    if (!drm_node || !*drm_node) {
        return false;
    }
    int fd = open(drm_node, O_RDWR);
    if (fd < 0) {
        return false;
    }
    VADisplay dpy = vaGetDisplayDRM(fd);
    int maj = 0, min = 0;
    bool cbr = false;
    if (dpy && vaInitialize(dpy, &maj, &min) == VA_STATUS_SUCCESS) {
        cbr = (vaapi_rc_mask(dpy) & VA_RC_CBR) != 0;
        vaTerminate(dpy);
    }
    close(fd);
    return cbr;
}

bool ww_enc_probe_444(const char *kind, const char *drm_node)
{
    (void)drm_node;
    if (!kind || strcmp(kind, "nvenc") != 0) {
        return false;   /* VAAPI 路径恒不支持 */
    }
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) {
        return false;
    }
    AVCodecContext *ctx = avcodec_alloc_context3(codec);
    if (!ctx) {
        return false;
    }
    ctx->width = 256;
    ctx->height = 256;
    ctx->time_base = (AVRational){1, 60};
    ctx->framerate = (AVRational){60, 1};
    ctx->pix_fmt = AV_PIX_FMT_YUV444P;
    ctx->profile = FF_PROFILE_H264_HIGH_444_PREDICTIVE;
    ctx->max_b_frames = 0;
    av_opt_set(ctx->priv_data, "preset", "p4", 0);
    av_opt_set(ctx->priv_data, "tune", "ull", 0);
    av_opt_set(ctx->priv_data, "rc", "constqp", 0);
    av_opt_set_int(ctx->priv_data, "qp", 24, 0);
    bool ok = avcodec_open2(ctx, codec, NULL) >= 0;
    if (!ok) {
        fprintf(stderr, "[enc] NVENC 试开 yuv444p 失败：该显卡不支持 H.264 4:4:4\n");
    }
    avcodec_free_context(&ctx);
    return ok;
}

int ww_enc_submit_dmabuf(struct ww_enc *e, int fd, uint32_t fourcc, uint32_t w, uint32_t h,
                         uint32_t stride, uint64_t modifier, int64_t pts_us, bool force_idr)
{
    if (!e || w != (uint32_t)e->w || h != (uint32_t)e->h) {
        return -1;
    }

    VASurfaceID src = import_dmabuf(e, fd, fourcc, w, h, stride, modifier);
    if (src == VA_INVALID_SURFACE) {
        return -1;
    }

    AVFrame *f = av_frame_alloc();
    if (!f) {
        vaDestroySurfaces(e->dpy, &src, 1);
        return -1;
    }
    if (av_hwframe_get_buffer(e->frames, f, 0) < 0) {
        av_frame_free(&f);
        vaDestroySurfaces(e->dpy, &src, 1);
        return -1;
    }

    VASurfaceID dst = (VASurfaceID)(uintptr_t)f->data[3];
    if (vpp_convert(e, src, dst) != 0) {
        av_frame_free(&f);
        vaDestroySurfaces(e->dpy, &src, 1);
        return -1;
    }
    vaDestroySurfaces(e->dpy, &src, 1);
    vaSyncSurface(e->dpy, dst);

    f->pts = pts_us;
    if (force_idr || e->needs_idr) {
        f->pict_type = AV_PICTURE_TYPE_I;
        f->flags |= AV_FRAME_FLAG_KEY;
        e->needs_idr = false;
    }

    int ret = avcodec_send_frame(e->ctx, f);
    av_frame_free(&f);
    if (ret < 0 && ret != AVERROR(EAGAIN)) {
        fprintf(stderr, "[enc] send_frame 失败: %d\n", ret);
        return -1;
    }
    return 0;
}

static int send_sw_frame(struct ww_enc *e, AVFrame *sw, int64_t pts_us, bool force_idr)
{
    uint64_t t0 = enc_now_us();
    AVFrame *f = av_frame_alloc();
    if (!f) {
        return -1;
    }
    if (av_hwframe_get_buffer(e->frames, f, 0) < 0) {
        av_frame_free(&f);
        return -1;
    }
    uint64_t t1 = enc_now_us();
    if (av_hwframe_transfer_data(f, sw, 0) < 0) {
        fprintf(stderr, "[enc] NV12 上传失败\n");
        av_frame_free(&f);
        return -1;
    }
    uint64_t t2 = enc_now_us();
    e->stat_up_us += (t1 - t0) + (t2 - t1);
    f->pts = pts_us;
    if (force_idr || e->needs_idr) {
        f->pict_type = AV_PICTURE_TYPE_I;
        f->flags |= AV_FRAME_FLAG_KEY;
        e->needs_idr = false;
    }
    int ret = avcodec_send_frame(e->ctx, f);
    e->stat_send_us += enc_now_us() - t2;
    e->stat_n++;
    av_frame_free(&f);
    return (ret < 0 && ret != AVERROR(EAGAIN)) ? -1 : 0;
}

/* 复用同一块系统内存帧；色度格式变化（重建编码器）时自动重分配 */
static AVFrame *ensure_sw_frame(struct ww_enc *e, int fmt)
{
    if (e->sw_frame && e->sw_frame->format != fmt) {
        av_frame_free(&e->sw_frame);
    }
    if (!e->sw_frame) {
        e->sw_frame = av_frame_alloc();
        if (!e->sw_frame) {
            return NULL;
        }
        e->sw_frame->format = fmt;
        e->sw_frame->width = e->w;
        e->sw_frame->height = e->h;
        if (av_frame_get_buffer(e->sw_frame, 32) < 0) {
            av_frame_free(&e->sw_frame);
            return NULL;
        }
    }
    return e->sw_frame;
}

/* NVENC 直接吃系统内存帧（NV12/YUV444P），由驱动自行上传 */
static int send_sw_direct(struct ww_enc *e, AVFrame *sw, int64_t pts_us, bool force_idr)
{
    sw->pts = pts_us;
    if (force_idr || e->needs_idr) {
        sw->pict_type = AV_PICTURE_TYPE_I;
        sw->flags |= AV_FRAME_FLAG_KEY;
        e->needs_idr = false;
    } else {
        sw->pict_type = AV_PICTURE_TYPE_NONE;
        sw->flags &= ~AV_FRAME_FLAG_KEY;
    }
    int ret = avcodec_send_frame(e->ctx, sw);
    e->stat_n++;
    return (ret < 0 && ret != AVERROR(EAGAIN)) ? -1 : 0;
}

int ww_enc_submit_cpu(struct ww_enc *e, const void *ptr, uint32_t fourcc, uint32_t w, uint32_t h,
                      uint32_t stride, int64_t pts_us, bool force_idr)
{
    (void)fourcc;
    if (!e || !ptr || w != (uint32_t)e->w || h != (uint32_t)e->h) {
        return -1;
    }
    const int dst_fmt = e->chroma444 ? AV_PIX_FMT_YUV444P : AV_PIX_FMT_NV12;
    if (!ensure_sw_frame(e, dst_fmt)) {
        return -1;
    }
    if (!e->sws_mt[0]) {
        for (int i = 0; i < WW_SWS_THREADS; i++) {
            e->sws_mt[i] = sws_getContext(e->w, e->h, AV_PIX_FMT_BGRA, e->w, e->h,
                                          dst_fmt, SWS_POINT, NULL, NULL, NULL);
            if (!e->sws_mt[i]) {
                fprintf(stderr, "[enc] sws 初始化失败\n");
                return -1;
            }
        }
    }
    if (av_frame_make_writable(e->sw_frame) < 0) {
        return -1;
    }

#ifdef HAVE_LIBYUV
    uint64_t c0 = enc_now_us();
    /* libyuv 只有 NV12 输出；4:4:4 走下面的 sws 分带路径 */
    if (!e->chroma444 &&
        ww_argb_to_nv12((const unsigned char *)ptr, (int)stride,
                        e->sw_frame->data[0], e->sw_frame->linesize[0],
                        e->sw_frame->data[1], e->sw_frame->linesize[1],
                        e->w, e->h) == 0) {
        e->stat_conv_us += enc_now_us() - c0;

        if (e->kind == 1) {
            /* NVENC 直接吃系统内存 NV12，由它自己上传，无需 hwframe */
            return send_sw_direct(e, e->sw_frame, pts_us, force_idr);
        }
        return send_sw_frame(e, e->sw_frame, pts_us, force_idr);
    }
#endif

    /* 回退路径：sws 分 4 带并行 */
    pthread_t th[WW_SWS_THREADS];
    struct sws_band bands[WW_SWS_THREADS];
    int base = e->h / WW_SWS_THREADS;
    int rem = e->h % WW_SWS_THREADS;
    int y = 0;
    for (int i = 0; i < WW_SWS_THREADS; i++) {
        int rows = base + (i < rem ? 1 : 0);
        bands[i].ctx = e->sws_mt[i];
        bands[i].src = (const uint8_t *)ptr;   /* 传整帧基址，靠 srcSliceY 定位 */
        bands[i].src_stride = (int)stride;
        memcpy(bands[i].dst, e->sw_frame->data, sizeof(bands[i].dst));
        memcpy(bands[i].dst_stride, e->sw_frame->linesize, sizeof(bands[i].dst_stride));
        bands[i].y0 = y;
        bands[i].rows = rows;
        y += rows;
    }
    for (int i = 1; i < WW_SWS_THREADS; i++) {
        pthread_create(&th[i], NULL, sws_band_run, &bands[i]);
    }
    sws_band_run(&bands[0]);
    for (int i = 1; i < WW_SWS_THREADS; i++) {
        pthread_join(th[i], NULL);
    }
    if (e->kind == 1) {
        return send_sw_direct(e, e->sw_frame, pts_us, force_idr);
    }
    return send_sw_frame(e, e->sw_frame, pts_us, force_idr);
}

int ww_enc_submit_nv12(struct ww_enc *e, const uint8_t *y, int ys,
                       const uint8_t *uv, int uvs,
                       uint32_t w, uint32_t h, int64_t pts_us, bool force_idr)
{
    if (!e || !y || !uv || w != (uint32_t)e->w || h != (uint32_t)e->h) {
        return -1;
    }
    uint64_t t0 = enc_now_us();

    if (e->chroma444) {
        /* 零拷贝抓帧只产出 NV12，4:4:4 需要一次色度上采样 */
        AVFrame *f = ensure_sw_frame(e, AV_PIX_FMT_YUV444P);
        if (!f) {
            return -1;
        }
        if (!e->sws_nv12_444) {
            e->sws_nv12_444 = sws_getContext(e->w, e->h, AV_PIX_FMT_NV12,
                                             e->w, e->h, AV_PIX_FMT_YUV444P,
                                             SWS_POINT, NULL, NULL, NULL);
            if (!e->sws_nv12_444) {
                return -1;
            }
        }
        if (av_frame_make_writable(f) < 0) {
            return -1;
        }
        const uint8_t *sp[4] = { y, uv, NULL, NULL };
        int ss[4] = { ys, uvs, 0, 0 };
        sws_scale(e->sws_nv12_444, sp, ss, 0, e->h, f->data, f->linesize);
        e->stat_conv_us += enc_now_us() - t0;
        if (e->kind == 1) {
            return send_sw_direct(e, f, pts_us, force_idr);
        }
        return send_sw_frame(e, f, pts_us, force_idr);
    }

    if (!e->nv12_frame) {
        e->nv12_frame = av_frame_alloc();
        if (!e->nv12_frame) {
            return -1;
        }
        e->nv12_frame->format = AV_PIX_FMT_NV12;
        e->nv12_frame->width = e->w;
        e->nv12_frame->height = e->h;
        if (av_frame_get_buffer(e->nv12_frame, 32) < 0) {
            av_frame_free(&e->nv12_frame);
            return -1;
        }
    }
    AVFrame *f = e->nv12_frame;
    if (av_frame_make_writable(f) < 0) {
        return -1;
    }
    for (int r = 0; r < e->h; r++) {
        memcpy(f->data[0] + (size_t)r * f->linesize[0], y + (size_t)r * ys, (size_t)e->w);
    }
    for (int r = 0; r < e->h / 2; r++) {
        memcpy(f->data[1] + (size_t)r * f->linesize[1], uv + (size_t)r * uvs, (size_t)e->w);
    }
    e->stat_conv_us += enc_now_us() - t0;

    f->pts = pts_us;
    if (force_idr || e->needs_idr) {
        f->pict_type = AV_PICTURE_TYPE_I;
        f->flags |= AV_FRAME_FLAG_KEY;
        e->needs_idr = false;
    } else {
        f->pict_type = AV_PICTURE_TYPE_NONE;
        f->flags &= ~AV_FRAME_FLAG_KEY;
    }

    if (e->kind == 1) {
        /* NVENC 直接吃系统内存 NV12，由驱动自己上传 */
        int ret = avcodec_send_frame(e->ctx, f);
        e->stat_n++;
        return (ret < 0 && ret != AVERROR(EAGAIN)) ? -1 : 0;
    }
    return send_sw_frame(e, f, pts_us, force_idr);
}

int ww_enc_receive(struct ww_enc *e, struct ww_nal_out *out)
{
    AVPacket *pkt = av_packet_alloc();
    if (!pkt) {
        return 0;
    }
    int ret = avcodec_receive_packet(e->ctx, pkt);
    if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
        av_packet_free(&pkt);
        return 0;
    }
    if (ret < 0) {
        av_packet_free(&pkt);
        return -1;
    }

    /* 静态缓冲：调用方在下次 receive 前使用完毕 */
    static uint8_t *buf = NULL;
    static size_t cap = 0;
    if ((size_t)pkt->size > cap) {
        uint8_t *nb = realloc(buf, pkt->size + 1024);
        if (!nb) {
            av_packet_free(&pkt);
            return -1;
        }
        buf = nb;
        cap = pkt->size + 1024;
    }
    memcpy(buf, pkt->data, pkt->size);

    /* 诊断：SPS 里的 profile_idc 才是实际生效的画质档次（244 = 4:4:4） */
    static int last_prof = -1;
    for (size_t i = 0; i + 5 < (size_t)pkt->size; i++) {
        if (pkt->data[i] || pkt->data[i + 1]) continue;
        size_t off = pkt->data[i + 2] == 1 ? i + 3
                   : (pkt->data[i + 2] == 0 && pkt->data[i + 3] == 1) ? i + 4 : 0;
        if (!off || off + 1 >= (size_t)pkt->size) continue;
        if ((pkt->data[off] & 0x1f) == 7) {
            int prof = pkt->data[off + 1];
            if (prof != last_prof) {
                last_prof = prof;
                fprintf(stderr, "[enc] 码流 SPS profile_idc=%d\n", prof);
            }
            break;
        }
    }

    out->data = buf;
    out->len = pkt->size;
    out->pts_us = pkt->pts;
    out->keyframe = (pkt->flags & AV_PKT_FLAG_KEY) != 0;

    av_packet_free(&pkt);
    return 1;
}

int ww_enc_set_qp(struct ww_enc *e, int qp)
{
    if (!e || qp < 1 || qp > 51 || qp == e->qp) {
        return 0;
    }
    e->qp = qp;
    if (e->cbr) {
        /* CBR 下码率说话，QP 不再参与码控；调用方应把目标码率设为 0 再改 QP */
        return 0;
    }
    if (e->kind == 1) {
        /* NVENC 的 constqp 参数在私有的 priv_data 里 */
        av_opt_set_int(e->ctx->priv_data, "qp", qp, 0);
        e->needs_idr = true;
        return 0;
    }
    e->ctx->flags |= AV_CODEC_FLAG_QSCALE;
    e->ctx->global_quality = qp * FF_QP2LAMBDA;
    /* CQP 下改动 global_quality 需要重建编码器才能稳定生效，这里标记下次强制 IDR 平滑过渡 */
    e->needs_idr = true;
    return 0;
}

int64_t ww_enc_set_bitrate(struct ww_enc *e, int64_t bps)
{
    if (!e || !e->cbr || bps <= 0 || bps == e->bitrate_bps) {
        return e && e->cbr ? e->bitrate_bps : 0;
    }
    e->bitrate_bps = bps;
    e->ctx->bit_rate = bps;
    e->ctx->rc_max_rate = bps;
    e->ctx->rc_buffer_size = (int)bps;
    av_opt_set_int(e->ctx->priv_data, "bitrate", bps, 0);
    av_opt_set_int(e->ctx->priv_data, "maxrate", bps, 0);
    av_opt_set_int(e->ctx->priv_data, "bufsize", bps, 0);
    e->needs_idr = true;
    return e->bitrate_bps;
}

bool ww_enc_is_cbr(struct ww_enc *e)
{
    return e && e->cbr;
}

void ww_enc_destroy(struct ww_enc *e)
{
    if (!e) {
        return;
    }
    if (e->ctx) {
        avcodec_free_context(&e->ctx);
    }
    if (e->frames) {
        av_buffer_unref(&e->frames);
    }
    if (e->hwdev) {
        av_buffer_unref(&e->hwdev);
    }
    if (e->vpp_ready) {
        vaDestroyContext(e->dpy, e->vpp_ctx);
        vaDestroyConfig(e->dpy, e->vpp_cfg);
    }
    if (e->sws) {
        sws_freeContext(e->sws);
    }
    for (int i = 0; i < WW_SWS_THREADS; i++) {
        if (e->sws_mt[i]) {
            sws_freeContext(e->sws_mt[i]);
        }
    }
    if (e->sws_nv12_444) {
        sws_freeContext(e->sws_nv12_444);
    }
    if (e->sw_frame) {
        av_frame_free(&e->sw_frame);
    }
    if (e->nv12_frame) {
        av_frame_free(&e->nv12_frame);
    }
    free(e);
}
