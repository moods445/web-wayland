#ifndef WW_ENCODER_H
#define WW_ENCODER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* 编码输出：一段 Annex-B 码流（可能含多个 NAL，含 SPS/PPS） */
struct ww_nal_out {
    const uint8_t *data;
    size_t len;
    int64_t pts_us;
    bool keyframe;
};

struct ww_enc;

/* 色彩信令（决定 VUI 与色彩转换矩阵，客户端必须按同一组参数解码上屏）：
 *   WW_COLOR_601       BT.601 limited（旧默认，与 libyuv 的 ARGBToI420 一致）
 *   WW_COLOR_709       BT.709 limited
 *   WW_COLOR_709_FULL  BT.709 全范围（0~255），桌面截图的 RGB 值原样落地，
 *                      不会把纯黑压到 16、纯白压到 235 —— 这是"像本地桌面"的关键
 */
enum { WW_COLOR_601 = 0, WW_COLOR_709 = 1, WW_COLOR_709_FULL = 2 };

struct ww_enc_cfg {
    const char *kind;      /* "vaapi" | "nvenc" */
    const char *drm_node;
    int w, h, fps, qp;
    bool chroma444;
    int64_t bitrate_bps;
    int color;             /* WW_COLOR_* */
};

/* drm_node: 例如 /dev/dri/renderD129（必须与 compositor 同一 GPU） */
/* kind: "vaapi"（集显 iGPU，低功耗 VDEnc）| "nvenc"（独显，吞吐更高） */
struct ww_enc *ww_enc_create_ex(const char *kind, const char *drm_node,
                                int w, int h, int fps, int qp);
/* chroma444=true 时使用 yuv444p（H.264 High 4:4:4 Predictive，仅 NVENC 支持） */
struct ww_enc *ww_enc_create_chroma(const char *kind, const char *drm_node,
                                    int w, int h, int fps, int qp, bool chroma444);
/* bitrate_bps>0 且编码器支持 CBR 时按目标码率编码（此时 qp 不参与），
   否则回退纯 CQP（码率随画面浮动，由 ww_enc_is_cbr 告知实际模式） */
struct ww_enc *ww_enc_create_full(const char *kind, const char *drm_node,
                                  int w, int h, int fps, int qp, bool chroma444,
                                  int64_t bitrate_bps);
/* 带色彩参数的完整创建入口（原生客户端用 709 全范围 + 低 QP 追求视觉无损） */
struct ww_enc *ww_enc_create_cfg(const struct ww_enc_cfg *cfg);
/* 该编码器能否按目标码率（CBR）编码；集显 VDEnc 实测只有 CQP */
bool ww_enc_probe_cbr(const char *kind, const char *drm_node);
/* 探测该编码器能否实际打开 4:4:4（VAAPI 恒为 false） */
bool ww_enc_probe_444(const char *kind, const char *drm_node);
struct ww_enc *ww_enc_create(const char *drm_node, int w, int h, int fps, int qp);
void ww_enc_destroy(struct ww_enc *e);

/* 提交一个 dmabuf（ARGB/XRGB），若驱动支持 VPP 则 GPU 内转换，否则内部回退 CPU 转换 */
int ww_enc_submit_dmabuf(struct ww_enc *e, int fd, uint32_t fourcc, uint32_t w, uint32_t h,
                         uint32_t stride, uint64_t modifier, int64_t pts_us, bool force_idr);

/* 直接提交 CPU 可读的像素指针（gbm 常驻映射），走 sws 转换 + 上传 */
int ww_enc_submit_cpu(struct ww_enc *e, const void *ptr, uint32_t fourcc, uint32_t w, uint32_t h,
                      uint32_t stride, int64_t pts_us, bool force_idr);

/* 直接提交已转换好的 NV12（零拷贝抓帧路径；NVENC 由驱动自行上传到显存） */
int ww_enc_submit_nv12(struct ww_enc *e, const uint8_t *y, int ys,
                       const uint8_t *uv, int uvs,
                       uint32_t w, uint32_t h, int64_t pts_us, bool force_idr);

/* 取出一个已编码的访问单元，返回 1 表示有输出，0 表示暂无 */
int ww_enc_receive(struct ww_enc *e, struct ww_nal_out *out);

/* 运行时改 QP（自适应控制器用），立即生效；CBR 模式下返回 0 且不生效 */
int ww_enc_set_qp(struct ww_enc *e, int qp);

/* 运行时改目标码率（bps），返回实际生效的码率；0 表示该编码器只支持 CQP */
int64_t ww_enc_set_bitrate(struct ww_enc *e, int64_t bps);
/* true = 当前按目标码率（CBR）编码；false = 按 QP（CQP），码率随画面浮动 */
bool ww_enc_is_cbr(struct ww_enc *e);

/* 取走内部分段耗时（毫秒，调用后清零），用于定位瓶颈 */
void ww_enc_take_stats(struct ww_enc *e, double *conv_ms, double *up_ms, double *send_ms);

#endif /* WW_ENCODER_H */
