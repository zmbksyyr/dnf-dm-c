@echo off
rem ===========================================================================
rem  Local build.  CI (.github/workflows/build.yml) uses the same flags.
rem
rem  Paths are derived from this file's location, so a clone anywhere works.
rem  Output goes outside the tree:  <project>\tools\build.bat
rem                             -> <project>\..\_build\DNF_GM_C\
rem  Override with the DNF_GM_BUILD environment variable.
rem
rem  ASCII only: cmd.exe renders UTF-8 Chinese in a .bat through the OEM
rem  codepage and prints mojibake.
rem ===========================================================================
setlocal enabledelayedexpansion

for %%I in ("%~dp0..") do set "ROOT=%%~fI"
for %%I in ("%ROOT%\..\_build\DNF_GM_C") do set "OUT=%%~fI"
if defined DNF_GM_BUILD set "OUT=%DNF_GM_BUILD%"

rem --- locate zig: %ZIG% > PATH > a couple of usual install spots ------------
set "ZBIN="
if defined ZIG if exist "%ZIG%" set "ZBIN=%ZIG%"
if not defined ZBIN for /f "delims=" %%P in ('where zig 2^>nul') do if not defined ZBIN set "ZBIN=%%P"
if not defined ZBIN for %%C in ("C:\zig\zig.exe" "%LOCALAPPDATA%\zig\zig.exe") do if not defined ZBIN if exist "%%~C" set "ZBIN=%%~C"
if not defined ZBIN goto :nozig

set "TARGET=-target x86_64-windows-gnu"
rem  Warnings are errors: a warning that survives a commit is a warning nobody
rem  reads again.  Keep this list identical to CI.
set "WARN=-Wall -Wextra -Wshadow -Wconversion -Wcast-qual -Wpedantic"
set "WARN=%WARN% -Wstrict-prototypes -Wmissing-prototypes -Wmissing-declarations"
set "WARN=%WARN% -Wundef -Wpointer-arith -Wwrite-strings -Wvla -Wdouble-promotion"
set "WARN=%WARN% -Wold-style-definition -Wredundant-decls -Werror"
set "CFLAGS=%TARGET% -std=c11 -O2 -DNDEBUG -DUNICODE -D_UNICODE %WARN%"
rem One source list for both build systems: every src\*.c, no manual updates.
set "SRC="
for %%F in ("%ROOT%\src\*.c") do set "SRC=!SRC! "%%F""
set "LIBS=-lcomctl32 -lcomdlg32 -lshell32 -luser32 -lgdi32 -lole32 -lws2_32 -ladvapi32 -lmsimg32 -ldbghelp"

cd /d "%ROOT%" || goto :fail
if not exist "%OUT%" mkdir "%OUT%" 2>nul

rem A running exe cannot be overwritten; say so instead of failing at link time.
if exist "%OUT%\DNF_GM.exe" (
  del /q "%OUT%\DNF_GM.exe" 2>nul
  if exist "%OUT%\DNF_GM.exe" (
    echo [ERROR] DNF_GM.exe is running - close it first:
    echo         %OUT%\DNF_GM.exe
    exit /b 1
  )
)

echo [1/3] resource
"%ZBIN%" rc -fo "%OUT%\res.obj" src\res.rc
if errorlevel 1 goto :fail

echo [2/3] compile ^& link
"%ZBIN%" cc %CFLAGS% -Isrc -static -s -Wl,--subsystem,windows ^
  %SRC% "%OUT%\res.obj" %LIBS% -o "%OUT%\DNF_GM.exe"
if errorlevel 1 goto :fail

echo [3/3] done
for %%F in ("%OUT%\DNF_GM.exe") do echo      %%~fF  %%~zF bytes
echo.
echo   zig       %ZBIN%
echo   source    %ROOT%
echo   output    %OUT%
echo.
echo   Regression: python tools\regress.py --list
exit /b 0

:nozig
echo [ERROR] zig not found. Set ZIG to zig.exe, or put it on PATH.
echo         Also tried C:\zig\zig.exe and %LOCALAPPDATA%\zig\zig.exe
exit /b 1

:fail
echo [ERROR] build failed
exit /b 1
