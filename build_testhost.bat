@echo off
setlocal enabledelayedexpansion

REM ============================================================
REM  build_testhost.bat - Build & test FastBitCopy TestHost (Windows)
REM
REM  Reads ENGINE_ROOT from .env (if present), otherwise defaults
REM  to ..\UnrealEngine (sibling directory).
REM
REM  Usage:
REM    build_testhost.bat                     - Editor (Development)
REM    build_testhost.bat Game                - Game client (Development)
REM    build_testhost.bat Editor Shipping     - Editor (Shipping)
REM    build_testhost.bat Game Shipping       - Game client (Shipping)
REM    build_testhost.bat test                - Build Editor + run automation tests
REM ============================================================

set "SCRIPT_DIR=%~dp0"
set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

REM ---- Load .env if present ----
set "ENV_FILE=%SCRIPT_DIR%\.env"
if exist "%ENV_FILE%" (
    for /f "usebackq tokens=1,* delims==" %%A in ("%ENV_FILE%") do (
        REM Skip comments and blank lines
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

REM ---- Parse arguments ----
set "TARGET_TYPE=%~1"
set "CONFIG=%~2"

if /i "%TARGET_TYPE%"=="test" goto :RunTest

if "%TARGET_TYPE%"=="" set "TARGET_TYPE=Editor"
if "%CONFIG%"=="" set "CONFIG=Development"

if /i "%TARGET_TYPE%"=="Editor" (
    set "TARGET_NAME=FastBitCopyHostEditor"
) else if /i "%TARGET_TYPE%"=="Game" (
    set "TARGET_NAME=FastBitCopyHost"
) else (
    echo ERROR: Unknown target type: %TARGET_TYPE%
    echo Usage: %~nx0 [Editor^|Game^|test] [Development^|Shipping]
    exit /b 1
)

echo.
echo ============================================================
echo  ENGINE_ROOT : %ENGINE_ROOT%
echo  Target      : %TARGET_NAME%
echo  Platform    : Win64
echo  Config      : %CONFIG%
echo ============================================================
echo.

call "%BUILD_BAT%" %TARGET_NAME% Win64 %CONFIG% -project="%PROJECT%"
if errorlevel 1 (
    echo.
    echo BUILD FAILED
    exit /b 1
)

echo.
echo BUILD SUCCEEDED
exit /b 0

:RunTest
REM ---- Build Editor first ----
echo.
echo === Step 1/2: Building Editor target ===
echo.
call "%BUILD_BAT%" FastBitCopyHostEditor Win64 Development -project="%PROJECT%"
if errorlevel 1 (
echo BUILD FAILED - cannot run tests.
    exit /b 1
)

REM ---- Run automation tests ----
echo.
echo === Step 2/2: Running automation tests ===
echo.
set "EDITOR_CMD=%ENGINE_ROOT%\Engine\Binaries\Win64\UnrealEditor-Cmd.exe"
if not exist "%EDITOR_CMD%" (
    echo ERROR: UnrealEditor-Cmd.exe not found at %EDITOR_CMD%
    exit /b 1
)

REM Test report export dir (useful for CI artifacts)
set "REPORT_DIR=%SCRIPT_DIR%\TestHost\Saved\Automation\Reports"
if not exist "%REPORT_DIR%" mkdir "%REPORT_DIR%"

REM Headless flags: keep us off all UI/audio subsystems.
REM -NullRHI / -nosound / -nosplash mirror the Unix script and avoid Slate
REM side-effects in commandlet mode.
"%EDITOR_CMD%" "%PROJECT%" ^
    -ExecCmds="Automation RunTests FastBitCopy; Quit" ^
    -TestExit="Automation Test Queue Empty" ^
    -ReportExportPath="%REPORT_DIR%" ^
    -unattended -NoPause -NullRHI -nosound -nosplash -nop4 -NoSourceControl -log
set "TEST_EXIT=%errorlevel%"

if not "%TEST_EXIT%"=="0" (
    echo.
    echo TESTS FAILED (exit code %TEST_EXIT%)
    echo Report dir: %REPORT_DIR%
    exit /b %TEST_EXIT%
)

echo.
echo ALL TESTS PASSED
echo Report dir: %REPORT_DIR%
exit /b 0
