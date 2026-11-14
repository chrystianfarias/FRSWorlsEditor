@echo off
setlocal
cd /d "%~dp0"
rem Builds FRSStreamLog.asi (x86, Release) into build\Release\.
rem Install by copying it into <game>\scripts\ (see README.md).

set "PF86=%ProgramFiles(x86)%"
set "VSWHERE=%PF86%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
  echo [error] vswhere not found
  exit /b 1
)

for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * ^
  -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 ^
  -property installationPath`) do set "VSPATH=%%i"
if not defined VSPATH (
  echo [error] Visual Studio with the C++ toolset not found
  exit /b 1
)

set "CMAKE=%VSPATH%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if not exist "%CMAKE%" set "CMAKE=cmake"

if not exist "build\CMakeCache.txt" (
  "%CMAKE%" -S . -B build -G "Visual Studio 17 2022" -A Win32
  if errorlevel 1 exit /b 1
)

"%CMAKE%" --build build --config Release
if errorlevel 1 (
  echo [error] build failed
  exit /b 1
)

echo [ok] %~dp0build\Release\FRSStreamLog.asi
