# Velox: ゲストの Vulkan ドライバー（Mesa の gfxstream）が AOSP の Android.bp で
# リンクする goldfish 系の依存を、NDK で静的ライブラリにまとめる。
#
# 対象（AOSP の Android.bp での名前）:
#   libgfxstream_common_logging, libOpenglCodecCommon（Vulkan が使う部分）,
#   libgfxstream_androidemu_static（Process/Tracing）, libqemupipe.ranchu,
#   libgfxstream_rcEnc_static, libbase（GetProperty の代替）
#
# プラットフォームの C++ ライブラリ（libbase.so など）にはリンクしない。libc++ は NDK の静的版。
project('velox-goldfish-deps', ['c', 'cpp'],
  default_options : ['cpp_std=c++17', 'c_std=c11', 'default_library=static',
                     'warning_level=1', 'b_staticpic=true'])

gfx = get_option('gfxstream_dir')
mesa = get_option('mesa_dir')

inc = include_directories(
  'shim/include',
  gfx / 'common/logging/include',
  gfx / 'guest/OpenglCodecCommon',
  gfx / 'guest/OpenglCodecCommon/include',
  gfx / 'guest/android-emu',
  gfx / 'guest/qemupipe/include',
  gfx / 'guest/qemupipe/include-types',
  gfx / 'guest/renderControl_enc',
  gfx / 'third_party/opengl/include',
  mesa / 'src/gfxstream/guest/iostream/include',
  mesa / 'src/gfxstream/guest/connection-manager',
  mesa / 'src/gfxstream/guest/GoldfishAddressSpace/include',
  mesa / 'src/gfxstream/guest/platform/include',
  mesa / 'src/gfxstream/aemu/include',
  mesa / 'src',
  mesa / 'include',
  is_system : false,
)

args = [
  '-DVK_USE_PLATFORM_ANDROID_KHR',
  '-DGFXSTREAM_ENABLE_GUEST_GOLDFISH',
  '-D_FILE_OFFSET_BITS=64',
  '-Wno-unused-parameter',
  '-Wno-unused-function',
  '-Wno-unused-variable',
  '-Wno-unused-private-field',
  '-Wno-missing-field-initializers',
  '-fvisibility=hidden',
]

srcs = files(
  'shim/properties.cpp',
  gfx / 'common/logging/logging.cpp',
  gfx / 'guest/OpenglCodecCommon/ChecksumCalculator.cpp',
  gfx / 'guest/OpenglCodecCommon/glUtils.cpp',
  gfx / 'guest/OpenglCodecCommon/EncoderDebug.cpp',
  gfx / 'guest/android-emu/aemu/base/Process.cpp',
  gfx / 'guest/android-emu/aemu/base/Tracing.cpp',
  gfx / 'guest/qemupipe/qemu_pipe_common.cpp',
  gfx / 'guest/qemupipe/qemu_pipe_guest.cpp',
  gfx / 'guest/renderControl_enc/renderControl_client_context.cpp',
  gfx / 'guest/renderControl_enc/renderControl_enc.cpp',
  gfx / 'guest/renderControl_enc/ExtendedRenderControl.cpp',
  gfx / 'guest/renderControl_enc/GfxStreamRenderControl.cpp',
  gfx / 'guest/renderControl_enc/GfxStreamRenderControlConnection.cpp',
)

static_library('velox_goldfish', srcs,
  include_directories : inc,
  c_args : args,
  cpp_args : args + ['-DLOG_TAG="gfxstream"'],
  install : false,
)
