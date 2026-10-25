@echo off
rem Local MSVC build: AutoAdjust.dll for AviSynth+ and VapourSynth (x64) in build\
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
set FLAGS=/nologo /O2 /EHsc /std:c++17 /fp:precise /W4 /MT /DNDEBUG /Iinclude\avisynth /Iinclude\vapoursynth /Isrc
rem Only the AVX2 kernels are built with AVX2 enabled; the dispatcher picks them at runtime.
cl %FLAGS% /arch:AVX2 /c src\core\kernels_avx2.cpp /Fobuild\kernels_avx2.obj || exit /b 1
cl %FLAGS% /LD ^
  src\core\autoadjust.cpp src\core\kernels_c.cpp src\core\kernels_sse2.cpp src\core\kernels_neon.cpp ^
  src\core\kernels_dispatch.cpp src\avs_plugin.cpp src\vs_plugin.cpp build\kernels_avx2.obj ^
  /Fobuild\ /Fdbuild\ /Febuild\AutoAdjust.dll /link /NOLOGO
