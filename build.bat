@echo off
rem build.bat - build tern.exe with MSVC (run from a Developer Command Prompt,
rem or after ilammy/msvc-dev-cmd in CI). Requires Visual Studio 2019 16.8+.
rem
rem   build.bat          build tern.exe from the single translation unit src\unity.c
rem   build.bat test     also build and run the unit tests and the golden tests
rem
rem src\std_embed.c is committed, so no generator step is needed here.

setlocal
set TERN_CFLAGS=/nologo /std:c11 /W4 /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS
if not exist build mkdir build

cl %TERN_CFLAGS% src\unity.c /Fe:tern.exe /Fo:build\unity.obj
if errorlevel 1 exit /b 1

if /i not "%~1"=="test" exit /b 0

cl %TERN_CFLAGS% tests\unit\test_base.c /Fe:build\test_base.exe /Fo:build\test_base.obj
if errorlevel 1 exit /b 1
cl %TERN_CFLAGS% tests\unit\test_sim.c /Fe:build\test_sim.exe /Fo:build\test_sim.obj
if errorlevel 1 exit /b 1
cl %TERN_CFLAGS% tests\unit\test_pkg.c /Fe:build\test_pkg.exe /Fo:build\test_pkg.obj
if errorlevel 1 exit /b 1
cl %TERN_CFLAGS% tests\unit\test_tools.c /Fe:build\test_tools.exe /Fo:build\test_tools.obj
if errorlevel 1 exit /b 1
cl %TERN_CFLAGS% tests\unit\test_fmt.c /Fe:build\test_fmt.exe /Fo:build\test_fmt.obj
if errorlevel 1 exit /b 1
cl %TERN_CFLAGS% tests\run.c /Fe:tests\run.exe /Fo:build\run.obj
if errorlevel 1 exit /b 1

build\test_base.exe
if errorlevel 1 exit /b 1
build\test_sim.exe
if errorlevel 1 exit /b 1
build\test_pkg.exe
if errorlevel 1 exit /b 1
build\test_tools.exe
if errorlevel 1 exit /b 1
build\test_fmt.exe
if errorlevel 1 exit /b 1
tests\run.exe --tern tern.exe
if errorlevel 1 exit /b 1
exit /b 0
