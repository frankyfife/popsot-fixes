@echo off
rem Builds build\dx.dll (32-bit) with the Visual Studio 2022 Build Tools.
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
  set "VS_PATH=%%i"
)
call "%VS_PATH%\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1

cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /LD /EHsc /W3 /Fo:build\ src\proxy.cpp src\menupad.cpp src\trace.cpp src\consolemenu.cpp src\gamepad.cpp src\menucam.cpp src\sound.cpp src\video.cpp src\ui.cpp src\texpack.cpp src\cheats.cpp /Fe:build\dx.dll /link /DEF:src\dx.def user32.lib ole32.lib mfplat.lib mfreadwrite.lib mfuuid.lib
