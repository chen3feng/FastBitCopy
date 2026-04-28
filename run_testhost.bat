@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM  run_testhost.bat — Build & run FastBitCopy TestHost tests (Windows)
REM
REM  Covers both Editor (dynamic linking) and Game (static linking) targets.
REM
REM  Reads ENGINE_ROOT from .env (if present), otherwise defaults
REM  to ..\UnrealEngine (sibling directory).
REM
REM  Usage:
REM    run_testhost.bat              — Run all (Editor build+test, Game build)
REM    run_testhost.bat editor       — Editor only: build + automation tests
REM    run_testhost.bat game         — Game only: build (link verification)
REM    run_testhost.bat --no-build   — Skip build, run Editor tests only
REM ============================================================

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

REM ---- Load .env if present ----
set "ENV_FILE=%SCRIPT_DIR%\.env"
if exist "%ENV_FILE%" (
    for /f "usebackq tokens=1,* delims==" %%A in ("%ENV_FILE%") do (
        set "LINE=%%A"
        if not "!LINE:~0,1!"=="#" (
            if not "%%A"=="" set "%%A=%%B"
        )
    )
)

REM ---- Default ENGINE_ROOT to sibling UnrealEngine ----
if not defined ENGINE_ROOT (
    set "ENGINE_ROOT=%SCRIPT_DIR%\..\UnrealEngine"
)

REM ---- Resolve to absolute path ----
pushd "%ENGINE_ROOT%" 2>nul
if errorlevel 1 (
    echo ERROR: ENGINE_ROOT directory not found: %ENGINE_ROOT%
    echo.
    echo Set ENGINE_ROOT in .env or ensure UnrealEngine is a sibling directory.
    exit /b 1
)
set "ENGINE_ROOT=%CD%"
popd

set "BUILD_BAT=%ENGINE_ROOT%\Engine\Build\BatchFiles\Build.bat"
if not exist "%BUILD_BAT%" (
    echo ERROR: Build.bat not found at %BUILD_BAT%
    exit /b 1
)

set "PROJECT=%SCRIPT_DIR%\TestHost\FastBitCopyHost.uproject"
if not exist "%PROJECT%" (
    echo ERROR: TestHost project not found at %PROJECT%
    exit /b 1
)

REM ---- Ensure .plugin_root junction exists ----
set "PLUGIN_ROOT=%SCRIPT_DIR%\TestHost\.plugin_root"
if not exist "%PLUGIN_ROOT%\FastBitCopy.uplugin" (
    if exist "%PLUGIN_ROOT%" rd /s /q "%PLUGIN_ROOT%" 2>nul
    mklink /J "%PLUGIN_ROOT%" "%SCRIPT_DIR%"
    if errorlevel 1 (
        echo ERROR: Failed to create junction %PLUGIN_ROOT% -^> %SCRIPT_DIR%
        echo        Try running as Administrator, or create it manually:
        echo        mklink /J "%PLUGIN_ROOT%" "%SCRIPT_DIR%"
        exit /b 1
    )
    echo Created junction: %PLUGIN_ROOT% -^> %SCRIPT_DIR%
)

set "EDITOR_CMD=%ENGINE_ROOT%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"

REM ---- Parse arguments ----
set "MODE=%~1"
if "%MODE%"=="" set "MODE=all"

if /i "%MODE%"=="--no-build" goto :RunTestsOnly
if /i "%MODE%"=="editor"     goto :EditorOnly
if /i "%MODE%"=="game"       goto :GameOnly
if /i "%MODE%"=="all"        goto :RunAll

echo ERROR: Unknown mode: %MODE%
echo Usage: %~nx0 [all^|editor^|game^|--no-build]
exit /b 1

REM ============================================================
:RunAll
REM ============================================================
set "TOTAL_STEPS=3"
set "FAILURES=0"

echo.
echo ############################################################
echo #  FastBitCopy TestHost — Full Test Suite (Windows)
echo #  ENGINE_ROOT: %ENGINE_ROOT%
echo ############################################################

REM ---- Step 1: Build Editor (dynamic linking) ----
echo.
echo === [1/%TOTAL_STEPS%] Building Editor target (dynamic linking) ===
echo.
call "%BUILD_BAT%" FastBitCopyHostEditor Win64 Development -project="%PROJECT%"
if errorlevel 1 (
    echo.
    echo [FAIL] Editor build failed
    set /a FAILURES+=1
    goto :SkipEditorTest
)
echo.
echo [PASS] Editor build succeeded

REM ---- Step 2: Run Editor automation tests ----
echo.
echo === [2/%TOTAL_STEPS%] Running Editor automation tests ===
echo.
if not exist "%EDITOR_CMD%" (
    echo [SKIP] UnrealEditor-Cmd.exe not found — cannot run automation tests
    set /a FAILURES+=1
    goto :SkipEditorTest
)
"%EDITOR_CMD%" "%PROJECT%" -ExecCmds="Automation RunTests FastBitCopy" -unattended -NoPause -NullRHI -log
if errorlevel 1 (
    echo.
    echo [FAIL] Editor automation tests failed
    set /a FAILURES+=1
) else (
    echo.
    echo [PASS] Editor automation tests passed
)

:SkipEditorTest

REM ---- Step 3: Build Game (static linking) ----
echo.
echo === [3/%TOTAL_STEPS%] Building Game target (static linking) ===
echo.
call "%BUILD_BAT%" FastBitCopyHost Win64 Development -project="%PROJECT%"
if errorlevel 1 (
    echo.
    echo [FAIL] Game build failed
    set /a FAILURES+=1
) else (
    echo.
echo [PASS] Game build succeeded ^(static link verification^)
)

goto :Summary

REM ============================================================
:EditorOnly
REM ============================================================
set "FAILURES=0"

echo.
echo ############################################################
echo #  FastBitCopy TestHost — Editor Tests (Windows)
echo #  ENGINE_ROOT: %ENGINE_ROOT%
echo ############################################################

echo.
echo === [1/2] Building Editor target (dynamic linking) ===
echo.
call "%BUILD_BAT%" FastBitCopyHostEditor Win64 Development -project="%PROJECT%"
if errorlevel 1 (
    echo.
    echo [FAIL] Editor build failed — cannot run tests.
    exit /b 1
)
echo.
echo [PASS] Editor build succeeded

echo.
echo === [2/2] Running Editor automation tests ===
echo.
if not exist "%EDITOR_CMD%" (
    echo ERROR: UnrealEditor-Cmd.exe not found at %EDITOR_CMD%
    exit /b 1
)
"%EDITOR_CMD%" "%PROJECT%" -ExecCmds="Automation RunTests FastBitCopy" -unattended -NoPause -NullRHI -log
if errorlevel 1 (
    echo.
    echo [FAIL] Editor automation tests failed
    set /a FAILURES+=1
) else (
    echo.
    echo [PASS] Editor automation tests passed
)

goto :Summary

REM ============================================================
:GameOnly
REM ============================================================
set "FAILURES=0"

echo.
echo ############################################################
echo #  FastBitCopy TestHost — Game Build (Windows)
echo #  ENGINE_ROOT: %ENGINE_ROOT%
echo ############################################################

echo.
echo === [1/1] Building Game target (static linking) ===
echo.
call "%BUILD_BAT%" FastBitCopyHost Win64 Development -project="%PROJECT%"
if errorlevel 1 (
    echo.
    echo [FAIL] Game build failed
    set /a FAILURES+=1
) else (
    echo.
echo [PASS] Game build succeeded ^(static link verification^)
)

goto :Summary

REM ============================================================
:RunTestsOnly
REM ============================================================
set "FAILURES=0"

echo.
echo ############################################################
echo #  FastBitCopy TestHost — Run Tests Only (Windows)
echo #  ENGINE_ROOT: %ENGINE_ROOT%
echo ############################################################

echo.
echo === [1/1] Running Editor automation tests (skip build) ===
echo.
if not exist "%EDITOR_CMD%" (
    echo ERROR: UnrealEditor-Cmd.exe not found at %EDITOR_CMD%
    exit /b 1
)
"%EDITOR_CMD%" "%PROJECT%" -ExecCmds="Automation RunTests FastBitCopy" -unattended -NoPause -NullRHI -log
if errorlevel 1 (
    echo.
    echo [FAIL] Editor automation tests failed
    set /a FAILURES+=1
) else (
    echo.
    echo [PASS] Editor automation tests passed
)

goto :Summary

REM ============================================================
:Summary
REM ============================================================
echo.
echo ############################################################
if %FAILURES% equ 0 (
    echo #  ALL PASSED
) else (
    echo #  %FAILURES% FAILURE(S)
)
echo ############################################################
echo.

exit /b %FAILURES%
