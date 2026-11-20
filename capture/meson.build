project('wwcap', ['c', 'cpp'],
  version: '0.1.0',
  default_options: ['c_std=c11', 'cpp_std=c++17', 'warning_level=1'])

cc = meson.get_compiler('c')
yuv_dep = cc.find_library('yuv', required: false)

wl_dep  = dependency('wayland-client')
gbm_dep = dependency('gbm')
drm_dep = dependency('libdrm')
va_dep  = dependency('libva')
# vaGetDisplayDRM（目标码率能力探测用 vaInitialize 查询码控属性）
va_drm_dep = cc.find_library('va-drm')
xkb_dep = dependency('xkbcommon')
avcodec_dep = dependency('libavcodec')
avutil_dep  = dependency('libavutil')
swscale_dep = dependency('libswscale')
threads_dep = dependency('threads')
# 音频：PipeWire 抓默认 sink 的 monitor，Opus 编码（48kHz 立体声 20ms 帧）。
# 硬依赖：缺 libpipewire-0.3-dev / libopus-dev 时直接构建失败，见 README 依赖行。
pipewire_dep = dependency('libpipewire-0.3')
opus_dep     = dependency('opus')

gen_src = files(
  'src/generated/ext-image-copy-capture-v1.c',
  'src/generated/ext-image-capture-source-v1.c',
  'src/generated/ext-foreign-toplevel-list-v1.c',
  'src/generated/linux-dmabuf-v1.c',
  'src/generated/viewporter.c',
  'src/generated/virtual-keyboard-unstable-v1.c',
  'src/generated/wlr-virtual-pointer-unstable-v1.c',
  'src/generated/wlr-screencopy-unstable-v1.c',
)

inc = include_directories('src', '../proto')

# ── 可选的零拷贝抓帧后端（独显）：screencopy dmabuf → EGL/GL shader → NV12 ──
egl_dep = dependency('egl', required: false)
gl_dep = dependency('gl', required: false)

have_glcapture = egl_dep.found() and gl_dep.found()

extra_src = []
extra_deps = []
extra_args = []
if have_glcapture
  extra_src += ['src/capture_sc.c']
  extra_deps += [egl_dep, gl_dep]
  extra_args += ['-DHAVE_SC_CAPTURE']
endif

executable('wwcap',
  ['src/main.c', 'src/encoder_vaapi.c', 'src/input.c', 'src/capture_shm.c', 'src/audio.c', 'src/yuv_shim.cc'] + extra_src + gen_src,
  dependencies: [wl_dep, gbm_dep, drm_dep, va_dep, va_drm_dep, xkb_dep, avcodec_dep, avutil_dep, swscale_dep, threads_dep, yuv_dep, pipewire_dep, opus_dep] + extra_deps,
  include_directories: inc,
  c_args: (yuv_dep.found() ? ['-DHAVE_LIBYUV'] : []) + extra_args,
  install: false,
)
