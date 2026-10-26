project('AutoAdjust', 'cpp',
  default_options : ['buildtype=release', 'b_ndebug=if-release', 'cpp_std=c++17'],
  meson_version : '>=0.64.0',
  version : '0.2.1'
)

kernel_sources = [
  'src/core/kernels_c.cpp',
  'src/core/kernels_sse2.cpp',
  'src/core/kernels_neon.cpp',
  'src/core/kernels_dispatch.cpp',
]
sources = ['src/core/autoadjust.cpp', 'src/avs_plugin.cpp', 'src/vs_plugin.cpp'] + kernel_sources

# Vendored AviSynth+ and VapourSynth (API 4) headers.
incs = include_directories('include/avisynth', 'include/vapoursynth', 'src')

cxx = meson.get_compiler('cpp')
msvc = cxx.get_argument_syntax() == 'msvc'
if msvc
  add_project_arguments('/fp:precise', language : 'cpp')
else
  # Bit-exact output needs strict single precision: no fused multiply-add, no x87 excess precision.
  add_project_arguments('-Wall', '-Wextra', '-ffp-contract=off', language : 'cpp')
  if host_machine.cpu_family() in ['x86', 'x86_64']
    add_project_arguments('-mfpmath=sse', '-msse2', language : 'cpp')
  endif
endif

# The AVX2 kernels get AVX2 (but no FMA) only for their own file; asm_opt selects them at runtime.
# On other CPUs the file is empty, kernels_neon.cpp holds the arm64 kernels (NEON is part of arm64).
kernel_libs = []
if host_machine.cpu_family() in ['x86', 'x86_64']
  kernel_libs += static_library('kernels_avx2', 'src/core/kernels_avx2.cpp',
    cpp_args : msvc ? ['/arch:AVX2'] : ['-mavx2'],
    pic : true
  )
else
  kernel_sources += 'src/core/kernels_avx2.cpp'
  sources += 'src/core/kernels_avx2.cpp'
endif

# VapourSynth only autoloads *.dylib on macOS, while Meson defaults to *.so for modules.
module_kwargs = {}
if host_machine.system() == 'darwin'
  module_kwargs += {'name_suffix' : 'dylib'}
endif

if get_option('python_package')
  # Same layout as the VapourSynth PyPI package autoloads from: site-packages/vapoursynth/plugins
  py = import('python').find_installation(pure : false)
  install_dir = join_paths(py.get_install_dir(), 'vapoursynth', 'plugins')
else
  install_dir = join_paths(get_option('prefix'), get_option('libdir'), 'vapoursynth')
endif

shared_module('AutoAdjust', sources,
  include_directories : incs,
  link_with : kernel_libs,
  install : true,
  install_dir : install_dir,
  gnu_symbol_visibility : 'hidden',
  kwargs : module_kwargs
)

# Every kernel set against the C kernels on random data: meson test -C build
kernel_test = executable('kernel_test', ['test/kernel_test.cpp'] + kernel_sources,
  include_directories : incs,
  link_with : kernel_libs,
  build_by_default : false
)
test('kernels', kernel_test)
