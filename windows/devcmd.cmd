@echo off
setlocal
for /f "usebackq tokens=*" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "WVM_VS_ROOT=%%i"
if not defined WVM_VS_ROOT exit /b 1
call "%WVM_VS_ROOT%\Common7\Tools\VsDevCmd.bat" -arch=x64 >nul || exit /b 1
cd /d "%~dp0" || exit /b 1
%*
