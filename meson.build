project('AutoAdjust', 'cpp',
  default_options : ['buildtype=release', 'b_ndebug=if-release', 'cpp_std=c++17'],
  meson_version : '>=0.64.0',
  version : '0.1.0'
)

sources = [
  'src/core/autoadjust.cpp',
  'src/avs_plugin.cpp',
  'src/vs_plugin.cpp',
]

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
  install : true,
  install_dir : install_dir,
  gnu_symbol_visibility : 'hidden',
  kwargs : module_kwargs
)
