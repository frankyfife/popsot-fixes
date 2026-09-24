@echo off
rem Builds build\dx.dll (32-bit) with the Visual Studio 2022 Build Tools.
call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat" >nul || exit /b 1
cd /d "%~dp0"
if not exist build mkdir build
cl /nologo /O2 /MT /LD /EHsc /W3 /Fo:build\ src\proxy.cpp src\menupad.cpp src\trace.cpp src\consolemenu.cpp src\gamepad.cpp src\menucam.cpp /Fe:build\dx.dll /link /DEF:src\dx.def user32.lib
