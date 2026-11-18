project('wwcap', ['c', 'cpp'],
  version: '0.1.0',
  default_options: ['c_std=c11', 'cpp_std=c++17', 'warning_level=1'])

cc = meson.get_compiler('c')
yuv_dep = cc.find_library('yuv', required: false)

wl_dep  = dependency('wayland-client')
gbm_dep = dependency('gbm')
drm_dep = dependency('libdrm')
va_dep  = dependency('libva')
xkb_dep = dependency('xkbcommon')
avcodec_dep = dependency('libavcodec')
avutil_dep  = dependency('libavutil')
swscale_dep = dependency('libswscale')
threads_dep = dependency('threads')

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

executable('wwcap',
  ['src/main.c', 'src/encoder_vaapi.c', 'src/input.c', 'src/capture_shm.c', 'src/yuv_shim.cc'] + gen_src,
  dependencies: [wl_dep, gbm_dep, drm_dep, va_dep, xkb_dep, avcodec_dep, avutil_dep, swscale_dep, threads_dep, yuv_dep],
  include_directories: inc,
  c_args: yuv_dep.found() ? ['-DHAVE_LIBYUV'] : [],
  install: false,
)
