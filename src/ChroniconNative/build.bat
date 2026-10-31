@echo off
rem Builds ChroniconPlus's native parts with MSVC (x64, static CRT):
rem   build\version.dll               the loader Chronicon.exe picks up from its folder
rem   build\ChroniconPlus.Core.dll    the core it starts from <game>\ChroniconPlus\
rem   build\hook_test.exe             decoder and hook engine tests (run it after a build)
setlocal
set "HERE=%~dp0"
set "OUT=%HERE%build"
if not exist "%OUT%" mkdir "%OUT%"
if not exist "%OUT%\obj" mkdir "%OUT%\obj"

if defined VSCMD_VER goto :have_msvc
for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if not defined VSDIR ( echo MSVC not found & exit /b 1 )
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1
:have_msvc

set "CFLAGS=/nologo /O2 /MT /W3 /permissive- /std:c++20 /utf-8 /EHsc /DNDEBUG /DWIN32_LEAN_AND_MEAN /DNOMINMAX"

echo [1/3] loader (version.dll)
ml64 /nologo /c /Fo"%OUT%\obj\version_thunks.obj" "%HERE%proxy\version_thunks.asm" || exit /b 1
cl %CFLAGS% /DUNICODE /D_UNICODE /c /Fo"%OUT%\obj\version_proxy.obj" "%HERE%proxy\version_proxy.cpp" || exit /b 1
rc /nologo /fo"%OUT%\obj\version.res" "%HERE%proxy\version.rc" || exit /b 1
link /nologo /DLL /OUT:"%OUT%\version.dll" /DEF:"%HERE%proxy\version.def" "%OUT%\obj\version_thunks.obj" "%OUT%\obj\version_proxy.obj" "%OUT%\obj\version.res" kernel32.lib || exit /b 1

echo [2/3] core (ChroniconPlus.Core.dll)
set "CORE_SRC=x64_decode hook runner Core Commands Stats Menu Session Trace Features Autotest main"
set "CORE_CPP="
set "CORE_OBJ="
for %%f in (%CORE_SRC%) do call set "CORE_CPP=%%CORE_CPP%% "%HERE%core\%%f.cpp""
for %%f in (%CORE_SRC%) do call set "CORE_OBJ=%%CORE_OBJ%% "%OUT%\obj\%%f.obj""
cl %CFLAGS% /MP4 /c /Fo"%OUT%\obj\\" %CORE_CPP% || exit /b 1
rc /nologo /fo"%OUT%\obj\core.res" "%HERE%core\core.rc" || exit /b 1
link /nologo /DLL /MAP:"%OUT%\ChroniconPlus.Core.map" /OUT:"%OUT%\ChroniconPlus.Core.dll" %CORE_OBJ% "%OUT%\obj\core.res" kernel32.lib user32.lib || exit /b 1

echo [3/3] tests (hook_test.exe)
ml64 /nologo /c /Fo"%OUT%\obj\test_targets.obj" "%HERE%tests\test_targets.asm" || exit /b 1
cl %CFLAGS% /c /Fo"%OUT%\obj\hook_test.obj" "%HERE%tests\hook_test.cpp" || exit /b 1
link /nologo /OUT:"%OUT%\hook_test.exe" "%OUT%\obj\hook_test.obj" "%OUT%\obj\test_targets.obj" "%OUT%\obj\hook.obj" "%OUT%\obj\x64_decode.obj" kernel32.lib || exit /b 1

echo done: %OUT%
