@echo off
setlocal
rem Builds build\dx.dll (32-bit) with Visual Studio 2022 or newer - the Build Tools or
rem any edition with the C++ x86/x64 tools. vswhere finds the installation.
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VS_PATH="
if exist "%VSWHERE%" for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_PATH=%%i"
if not defined VS_PATH (
    echo Visual Studio with the C++ x86/x64 tools not found.
    exit /b 1
)
call "%VS_PATH%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /LD /EHsc /W3 /Fo:build\ src\proxy.cpp src\menupad.cpp src\trace.cpp src\consolemenu.cpp src\gamepad.cpp src\menucam.cpp src\sound.cpp src\video.cpp src\ui.cpp src\texpack.cpp src\cheats.cpp src\water.cpp /Fe:build\dx.dll /link /DEF:src\dx.def user32.lib ole32.lib mfplat.lib mfreadwrite.lib mfuuid.lib
