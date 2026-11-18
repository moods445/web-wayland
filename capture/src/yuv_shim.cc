// libyuv 只有 C++ 接口，这里给 C 侧提供最小封装。
// NV12 + BT.601(limited)，并在编码器 VUI 里显式声明 SMPTE170M——
// 只说转换不说信令时，Chrome 对 HD 默认按 BT.709 解释，画面会发灰发闷。
#include <libyuv.h>

extern "C" int ww_argb_to_nv12(const unsigned char *src, int src_stride,
                               unsigned char *dst_y, int dst_stride_y,
                               unsigned char *dst_uv, int dst_stride_uv,
                               int width, int height)
{
    return libyuv::ARGBToNV12(src, src_stride, dst_y, dst_stride_y,
                              dst_uv, dst_stride_uv, width, height);
}
