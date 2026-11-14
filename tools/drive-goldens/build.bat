@echo off
rem Builds drive-goldens.exe: the C++ golden runner for src/drive (vehicle physics, numerics, camera).
rem   build.bat            -> build\drive-goldens.exe with every suite
rem   set OUT=dir          -> build into another folder (parallel builds)
rem   set SUITES=mover wheel ...  -> only these suite_<name>.cpp files (main.cpp always has the maths helpers)
rem   set EXTRA=/Od     -> extra compiler flags (flag-sensitivity checks)
rem Flags match the editor's src/drive build: x64, /fp:precise, /O2.
setlocal EnableDelayedExpansion
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul
cd /d %~dp0
set S=%~dp0..\..\src
set D=%S%\drive
if "%OUT%"=="" set OUT=build
if "%SUITES%"=="" set SUITES=mover wheel engine rigidbody timing keyboard camera
if not exist "%OUT%\obj" mkdir "%OUT%\obj"
set SRC=main.cpp
for %%s in (%SUITES%) do set SRC=!SRC! suite_%%s.cpp
cl /nologo /MP /std:c++17 /O2 /fp:precise /EHsc /W3 /permissive- /utf-8 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DUNICODE /D_UNICODE %EXTRA% ^
  /I"%S%" /I"%S%\..\third_party" ^
  !SRC! ^
  "%D%\numerics\*.cpp" "%D%\physics\*.cpp" "%D%\physics\collision\*.cpp" "%D%\physics\engine\*.cpp" ^
  "%D%\physics\input\*.cpp" "%D%\physics\rigidbody\*.cpp" "%D%\physics\timing\*.cpp" "%D%\physics\wheel\*.cpp" ^
  "%D%\camera\*.cpp" "%D%\cars\CarTypeInfoU2.cpp" "%D%\cars\CarBin.cpp" ^
  /Fo"%OUT%\obj\\" /Fe"%OUT%\drive-goldens.exe" /link /STACK:16777216
exit /b %ERRORLEVEL%
