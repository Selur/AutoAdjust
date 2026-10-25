@echo off
rem Local MSVC build: AutoAdjust.dll for AviSynth+ and VapourSynth (x64) in build\
setlocal
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /LD /O2 /EHsc /std:c++17 /fp:precise /W4 /MT /DNDEBUG ^
  /Iinclude\avisynth /Iinclude\vapoursynth /Isrc ^
  src\core\autoadjust.cpp src\avs_plugin.cpp src\vs_plugin.cpp ^
  /Fobuild\ /Fdbuild\ /Febuild\AutoAdjust.dll /link /NOLOGO
