@echo off
rem ===========================================================================
rem  Build "LanPaint", a .8bf filter that runs LanPaint on the layer.
rem
rem  Requires: Visual Studio Build Tools 2022 (MSVC + Windows SDK) and the
rem  Adobe Photoshop plug-in API headers:
rem      tools\affinity\sdk\photoshop\*.h
rem      tools\affinity\sdk\pica_sp\*.h
rem  Both live with the RRAZ filters; the paths below point there.
rem
rem  Output:  affinity\out\LanPaint.8bf
rem ===========================================================================

setlocal
set "HERE=%~dp0"
set "SDK=D:\Projects\IFAN\tools\affinity\sdk"
set "OUT=%HERE%out"

if not exist "%SDK%\photoshop\PIFilter.h" (
    echo [BUILD] Plug-in SDK headers not found in "%SDK%".
    exit /b 1
)

set "VCVARS=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
if defined VCINSTALLDIR goto :have_vc
if not exist "%VCVARS%" goto :no_vc
call "%VCVARS%" >nul
:have_vc
goto :build

:no_vc
echo [BUILD] vcvars64.bat not found. Open a "x64 Native Tools" prompt instead.
exit /b 1

:build
if not exist "%OUT%" mkdir "%OUT%"
pushd "%OUT%"

echo [BUILD] compiling resources...
rc /nologo /fo "%OUT%\lanpaint.res" "%HERE%lanpaint.rc"
if errorlevel 1 goto :failed

echo [BUILD] compiling and linking...
cl /nologo /LD /MT /O2 /W3 /D_CRT_SECURE_NO_WARNINGS /DMSWindows=1 ^
   /I"%SDK%\photoshop" /I"%SDK%\pica_sp" ^
   /Fo"%OUT%\lanpaint.obj" ^
   "%HERE%lanpaint.c" "%OUT%\lanpaint.res" ^
   /link /DLL /EXPORT:PluginMain /IMPLIB:"%OUT%\lanpaint.lib" ^
   /OUT:"%OUT%\LanPaint.8bf" user32.lib comctl32.lib
if errorlevel 1 goto :failed

del /q lanpaint.obj lanpaint.lib lanpaint.exp 2>nul
if not exist "%OUT%\lanpaint.ini" copy /y "%HERE%lanpaint.ini.example" "%OUT%\lanpaint.ini" >nul 2>nul
popd
echo.
echo [BUILD] ok: "%OUT%\LanPaint.8bf"
endlocal
exit /b 0

:failed
popd
endlocal
exit /b 1
