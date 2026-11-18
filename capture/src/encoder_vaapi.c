/*
 * VAAPI 编码器：dmabuf(ARGB) --VPP--> NV12 --h264_vaapi(VDEnc/CQP)--> Annex-B
 *
 * 关键点（均为本机实测得出）：
 *  - HD 630 的 H.264 只有 VAEntrypointEncSliceLP 入口，故必须 low_power=1
 *  - 该入口只支持 CQP 码控，没有 CBR/VBR，因此自适应策略调 QP
 *  - dmabuf 与 compositor 同卡（renderD129），VPP 与编码共用同一个 VADisplay，全程 GPU
 */

#include "encoder.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

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
    bool needs_idr;
    int kind;              /* 0 = vaapi（集显 VDEnc）, 1 = nvenc（独显） */
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

/* ── 独显后端：h264_nvenc（软件 NV12 帧直接送入，NVENC 自行上传）── */
static struct ww_enc *create_nvenc(struct ww_enc *e, int w, int h, int fps, int qp)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("h264_nvenc");
    if (!codec) {
        fprintf(stderr, "[enc] 没有 h264_nvenc（驱动或 ffmpeg 不支持）\n");
        return NULL;
    }
    e->kind = 1;
    e->ctx = avcodec_alloc_context3(codec);
    e->ctx->width = w;
    e->ctx->height = h;
    e->ctx->time_base = (AVRational){1, 1000000};
    e->ctx->framerate = (AVRational){fps, 1};
    e->ctx->pix_fmt = AV_PIX_FMT_NV12;
    e->ctx->profile = FF_PROFILE_H264_HIGH;
    e->ctx->max_b_frames = 0;
    e->ctx->gop_size = 120;
    /* NVENC 用 priv 的 qp（constqp），不要设 global_quality，否则会告警 */
    e->ctx->colorspace = AVCOL_SPC_SMPTE170M;
    e->ctx->color_primaries = AVCOL_PRI_SMPTE170M;
    e->ctx->color_trc = AVCOL_TRC_SMPTE170M;
    e->ctx->color_range = AVCOL_RANGE_MPEG;

    /* 低延迟组合：p4 + ull + 恒定 QP + 不排队 */
    av_opt_set(e->ctx->priv_data, "preset", "p4", 0);
    av_opt_set(e->ctx->priv_data, "tune", "ull", 0);
    av_opt_set(e->ctx->priv_data, "rc", "constqp", 0);
    av_opt_set_int(e->ctx->priv_data, "qp", qp, 0);
    av_opt_set_int(e->ctx->priv_data, "delay", 0, 0);
    av_opt_set_int(e->ctx->priv_data, "zerolatency", 1, 0);

    if (avcodec_open2(e->ctx, codec, NULL) < 0) {
        fprintf(stderr, "[enc] h264_nvenc 打开失败（QP=%d）\n", qp);
        return NULL;
    }
    fprintf(stderr, "[enc] h264_nvenc 就绪 %dx%d@%d qp=%d (constqp/ull)\n", w, h, fps, qp);
    return e;
}

struct ww_enc *ww_enc_create_ex(const char *kind, const char *drm_node,
                                int w, int h, int fps, int qp)
{
    struct ww_enc *e = calloc(1, sizeof(*e));
    if (!e) {
        return NULL;
    }
    e->w = w;
    e->h = h;
    e->fps = fps;
    e->qp = qp;

    if (kind && !strcmp(kind, "nvenc")) {
        if (!create_nvenc(e, w, h, fps, qp)) {
            free(e);
            return NULL;
        }
        return e;
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
        return NULL;
    }
    AVHWFramesContext *fc = (AVHWFramesContext *)e->frames->data;
    fc->format = AV_PIX_FMT_VAAPI;
    fc->sw_format = AV_PIX_FMT_NV12;
    fc->width = w;
    fc->height = h;
    fc->initial_pool_size = 16;
    if (av_hwframe_ctx_init(e->frames) < 0) {
        fprintf(stderr, "[enc] hwframe_ctx_init 失败\n");
        return NULL;
    }

    const AVCodec *codec = avcodec_find_encoder_by_name("h264_vaapi");
    if (!codec) {
        fprintf(stderr, "[enc] 找不到 h264_vaapi\n");
        return NULL;
    }
    e->ctx = avcodec_alloc_context3(codec);
    e->ctx->width = w;
    e->ctx->height = h;
    e->ctx->time_base = (AVRational){1, 1000000};
    e->ctx->framerate = (AVRational){fps, 1};
    e->ctx->pix_fmt = AV_PIX_FMT_VAAPI;
    e->ctx->profile = FF_PROFILE_H264_HIGH;
    e->ctx->max_b_frames = 0;
    e->ctx->gop_size = 120;
    /* 必须同时打上 QSCALE 标志，否则 global_quality 会被忽略（QP 永远停在驱动默认值） */
    e->ctx->flags |= AV_CODEC_FLAG_QSCALE;
    e->ctx->global_quality = qp * FF_QP2LAMBDA;
    e->ctx->hw_frames_ctx = av_buffer_ref(e->frames);
    e->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;

    if (av_opt_set(e->ctx->priv_data, "rc_mode", "CQP", 0) < 0) {
        fprintf(stderr, "[enc] 设置 rc_mode=CQP 失败\n");
    }
    av_opt_set_int(e->ctx->priv_data, "low_power", 1, 0);
    /* async_depth=1 会把「提交下一帧」与「编码上一帧」串行化；放开后可以重叠 */
    av_opt_set_int(e->ctx->priv_data, "async_depth", 8, 0);

    /* 信令必须与 libyuv 的 ARGBToI420 一致（BT.601 limited），否则解码端按 709 解释会发灰 */
    e->ctx->colorspace = AVCOL_SPC_SMPTE170M;
    e->ctx->color_primaries = AVCOL_PRI_SMPTE170M;
    e->ctx->color_trc = AVCOL_TRC_SMPTE170M;
    e->ctx->color_range = AVCOL_RANGE_MPEG;

    if (avcodec_open2(e->ctx, codec, NULL) < 0) {
        fprintf(stderr, "[enc] avcodec_open2 失败（QP=%d）\n", qp);
        return NULL;
    }

    if (vpp_init(e) != 0) {
        e->use_sws = true;
        fprintf(stderr, "[enc] 驱动未提供 VPP，回退 CPU 色彩转换（ARGB→NV12）\n");
    }

    fprintf(stderr, "[enc] h264_vaapi 就绪 %dx%d@%d qp=%d (VDEnc/CQP)\n", w, h, fps, qp);
    return e;
}

struct ww_enc *ww_enc_create(const char *drm_node, int w, int h, int fps, int qp)
{
    return ww_enc_create_ex("vaapi", drm_node, w, h, fps, qp);
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

static int send_sw_frame(struct ww_enc *e, int64_t pts_us, bool force_idr)
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
    if (av_hwframe_transfer_data(f, e->sw_frame, 0) < 0) {
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

int ww_enc_submit_cpu(struct ww_enc *e, const void *ptr, uint32_t fourcc, uint32_t w, uint32_t h,
                      uint32_t stride, int64_t pts_us, bool force_idr)
{
    (void)fourcc;
    if (!e || !ptr || w != (uint32_t)e->w || h != (uint32_t)e->h) {
        return -1;
    }
    if (!e->sw_frame) {
        e->sw_frame = av_frame_alloc();
        e->sw_frame->format = AV_PIX_FMT_NV12;
        e->sw_frame->width = e->w;
        e->sw_frame->height = e->h;
        if (av_frame_get_buffer(e->sw_frame, 32) < 0) {
            return -1;
        }
    }
    if (!e->sws_mt[0]) {
        for (int i = 0; i < WW_SWS_THREADS; i++) {
            e->sws_mt[i] = sws_getContext(e->w, e->h, AV_PIX_FMT_BGRA, e->w, e->h,
                                          AV_PIX_FMT_NV12, SWS_POINT, NULL, NULL, NULL);
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
    if (ww_argb_to_nv12((const unsigned char *)ptr, (int)stride,
                        e->sw_frame->data[0], e->sw_frame->linesize[0],
                        e->sw_frame->data[1], e->sw_frame->linesize[1],
                        e->w, e->h) == 0) {
        e->stat_conv_us += enc_now_us() - c0;
        e->sw_frame->pts = pts_us;

        if (e->kind == 1) {
            /* NVENC 直接吃系统内存 NV12，由它自己上传，无需 hwframe */
            if (force_idr || e->needs_idr) {
                e->sw_frame->pict_type = AV_PICTURE_TYPE_I;
                e->sw_frame->flags |= AV_FRAME_FLAG_KEY;
                e->needs_idr = false;
            } else {
                e->sw_frame->pict_type = AV_PICTURE_TYPE_NONE;
            }
            int ret = avcodec_send_frame(e->ctx, e->sw_frame);
            e->stat_n++;
            return (ret < 0 && ret != AVERROR(EAGAIN)) ? -1 : 0;
        }
        return send_sw_frame(e, pts_us, force_idr);
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
    e->sw_frame->pts = pts_us;
    return send_sw_frame(e, pts_us, force_idr);
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
    e->ctx->flags |= AV_CODEC_FLAG_QSCALE;
    e->ctx->global_quality = qp * FF_QP2LAMBDA;
    /* CQP 下改动 global_quality 需要重建编码器才能稳定生效，这里标记下次强制 IDR 平滑过渡 */
    e->needs_idr = true;
    return 0;
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
    if (e->sw_frame) {
        av_frame_free(&e->sw_frame);
    }
    free(e);
}
