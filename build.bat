@echo off
REM Build every stage of the mini database engine with MinGW g++.
REM Usage:  build.bat
setlocal

set FLAGS=-std=c++17 -O2 -Wall -Wextra -Wpedantic -Wshadow -Wformat=2 -Wcast-align -Wconversion -Wsign-conversion -Wnull-dereference

where g++ >nul 2>nul
if errorlevel 1 (
    echo g++ not found on PATH. Install MinGW-w64 and add its bin folder to PATH.
    exit /b 1
)

for %%S in (main main_stage2 main_stage3 main_stage3b main_stage4 tests main_repl) do (
    echo === building %%S ===
    g++ %FLAGS% %%S.cpp -o %%S.exe
    if errorlevel 1 (
        echo Build failed for %%S.
        exit /b 1
    )
)

echo.
echo All stages built. Run e.g.:  main_stage4.exe   (tests: tests.exe)
endlocal
