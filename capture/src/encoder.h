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

/* drm_node: 例如 /dev/dri/renderD129（必须与 compositor 同一 GPU） */
/* kind: "vaapi"（集显 iGPU，低功耗 VDEnc）| "nvenc"（独显，吞吐更高） */
struct ww_enc *ww_enc_create_ex(const char *kind, const char *drm_node,
                                int w, int h, int fps, int qp);
/* chroma444=true 时使用 yuv444p（H.264 High 4:4:4 Predictive，仅 NVENC 支持） */
struct ww_enc *ww_enc_create_chroma(const char *kind, const char *drm_node,
                                    int w, int h, int fps, int qp, bool chroma444);
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

/* 运行时改 QP（自适应控制器用），立即生效 */
int ww_enc_set_qp(struct ww_enc *e, int qp);

/* 取走内部分段耗时（毫秒，调用后清零），用于定位瓶颈 */
void ww_enc_take_stats(struct ww_enc *e, double *conv_ms, double *up_ms, double *send_ms);

#endif /* WW_ENCODER_H */
