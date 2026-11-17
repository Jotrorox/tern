@echo off
rem build.bat - build tern.exe with MSVC (run from a Developer Command Prompt,
rem or after ilammy/msvc-dev-cmd in CI). Requires Visual Studio 2019 16.8+.
rem
rem   build.bat          build tern.exe from the single translation unit src\unity.c
rem   build.bat test     also build and run the unit tests, the fuzz regression
rem                      corpus and the golden tests
rem
rem Warnings are errors (/W4 /WX), as -Wall -Wextra -Wpedantic -Werror are on
rem GCC and Clang (spec 13.7). MSVC has no C99 mode: the C dialect is
rem /std:c11 by default; set TERN_MSVC_STD=c17 for the C17 build (CI runs
rem both). No warning is switched off.
rem
rem src\std_embed.c is committed, so no generator step is needed here.

setlocal
if not defined TERN_MSVC_STD set TERN_MSVC_STD=c11
set TERN_CFLAGS=/nologo /std:%TERN_MSVC_STD% /W4 /WX /O2 /utf-8 /D_CRT_SECURE_NO_WARNINGS
if not exist build mkdir build

cl %TERN_CFLAGS% src\unity.c /Fe:tern.exe /Fo:build\unity.obj
if errorlevel 1 exit /b 1

if /i not "%~1"=="test" exit /b 0

cl %TERN_CFLAGS% tools\embed.c /Fe:build\embed.exe /Fo:build\embed.obj
if errorlevel 1 exit /b 1
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
cl %TERN_CFLAGS% tests\unit\test_emit.c /Fe:build\test_emit.exe /Fo:build\test_emit.obj
if errorlevel 1 exit /b 1
cl %TERN_CFLAGS% tests\unit\test_backend.c /Fe:build\test_backend.exe /Fo:build\test_backend.obj
if errorlevel 1 exit /b 1
cl %TERN_CFLAGS% tests\unit\test_lsp.c /Fe:build\test_lsp.exe /Fo:build\test_lsp.obj
if errorlevel 1 exit /b 1
cl %TERN_CFLAGS% tests\unit\test_elab.c /Fe:build\test_elab.exe /Fo:build\test_elab.obj
if errorlevel 1 exit /b 1
for %%h in (parser check toml json) do (
    cl %TERN_CFLAGS% /DFUZZ_STANDALONE tests\fuzz\fuzz_%%h.c /Fe:build\fuzz_replay_%%h.exe /Fo:build\fuzz_replay_%%h.obj
    if errorlevel 1 exit /b 1
)
cl %TERN_CFLAGS% tests\run.c /Fe:tests\run.exe /Fo:build\run.obj
if errorlevel 1 exit /b 1
cl %TERN_CFLAGS% tests\doc_snippets.c /Fe:build\doc_snippets.exe /Fo:build\doc_snippets.obj
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
tern.exe fmt --check std
if errorlevel 1 exit /b 1
build\test_emit.exe
if errorlevel 1 exit /b 1
build\test_backend.exe
if errorlevel 1 exit /b 1
build\test_lsp.exe
if errorlevel 1 exit /b 1
build\test_elab.exe tern.exe
if errorlevel 1 exit /b 1
for %%h in (parser check toml json) do (
    build\fuzz_replay_%%h.exe -q tests\fuzz\corpus\%%h
    if errorlevel 1 exit /b 1
)
tests\run.exe --tern tern.exe
if errorlevel 1 exit /b 1
build\doc_snippets.exe --tern tern.exe
if errorlevel 1 exit /b 1
exit /b 0
