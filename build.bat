@echo off
setlocal enabledelayedexpansion

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo vswhere.exe not found. Please run inside a Visual Studio Developer Command Prompt.
    exit /b 1
)

for /f "usebackq tokens=*" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do (
    set "VSINSTALL=%%i"
)

if "%VSINSTALL%"=="" (
    echo Visual Studio C++ Build Tools x64 not found.
    exit /b 1
)

call "%VSINSTALL%\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo Failed to initialize vcvars64.bat
    exit /b 1
)

if not exist build mkdir build

set /a CUR_BUILD=0
if exist res\build_number.txt (
    for /f "usebackq delims=" %%B in ("res\build_number.txt") do set /a CUR_BUILD=%%B
)
set /a NEXT_BUILD=CUR_BUILD+1
if !NEXT_BUILD! LSS 100000 (
    set "BUILD_PAD=00000!NEXT_BUILD!"
    set "BUILD_PAD=!BUILD_PAD:~-5!"
) else (
    set "BUILD_PAD=!NEXT_BUILD!"
)
set "BUILD_TAG=build-!BUILD_PAD!"

(
    echo #pragma once
    echo #define APP_BUILD_NUM !NEXT_BUILD!
    echo #define APP_BUILD_TAG_A "!BUILD_TAG!"
    echo #define APP_BUILD_TAG_W L"!BUILD_TAG!"
) > build\build_version.h

echo [0/3] Version: !BUILD_TAG!
echo [1/3] Compiling resources...
rc.exe /nologo /I src /I build /fo build\app.res res\app.rc
if errorlevel 1 exit /b 1

echo [2/3] Compiling C++ sources (/MT static CRT, /O2)...
cl.exe /nologo /c /utf-8 /std:c++17 /O2 /W3 /EHsc /MT /DUNICODE /D_UNICODE /DNOMINMAX /DWIN32_LEAN_AND_MEAN ^
    /I src /I build /Fobuild\ ^
    src\main.cpp ^
    src\core\settings.cpp ^
    src\core\theme.cpp ^
    src\core\process_monitor.cpp ^
    src\core\perf_monitor.cpp ^
    src\core\startup_manager.cpp ^
    src\core\service_manager.cpp ^
    src\core\user_manager.cpp ^
    src\core\app_history.cpp ^
    src\core\uwp_apps.cpp ^
    src\ui\listview_theme.cpp ^
    src\ui\dialogs.cpp ^
    src\ui\tab_processes.cpp ^
    src\ui\tab_performance.cpp ^
    src\ui\tab_app_history.cpp ^
    src\ui\tab_startup.cpp ^
    src\ui\tab_users.cpp ^
    src\ui\tab_details.cpp ^
    src\ui\tab_services.cpp ^
    src\ui\main_window.cpp
if errorlevel 1 exit /b 1

echo [3/3] Linking single-file Taskmgr.exe...
powershell -NoProfile -Command "$t = Join-Path (Get-Location) 'Taskmgr.exe'; Get-Process Taskmgr -ErrorAction SilentlyContinue | Where-Object { $_.Path -eq $t } | Stop-Process -Force -ErrorAction SilentlyContinue" >nul 2>&1
link.exe /nologo /SUBSYSTEM:WINDOWS /OPT:REF /OPT:ICF /OUT:Taskmgr.exe ^
    build\app.res ^
    build\main.obj ^
    build\settings.obj ^
    build\theme.obj ^
    build\process_monitor.obj ^
    build\perf_monitor.obj ^
    build\startup_manager.obj ^
    build\service_manager.obj ^
    build\user_manager.obj ^
    build\app_history.obj ^
    build\uwp_apps.obj ^
    build\listview_theme.obj ^
    build\dialogs.obj ^
    build\tab_processes.obj ^
    build\tab_performance.obj ^
    build\tab_app_history.obj ^
    build\tab_startup.obj ^
    build\tab_users.obj ^
    build\tab_details.obj ^
    build\tab_services.obj ^
    build\main_window.obj ^
    user32.lib gdi32.lib gdiplus.lib comctl32.lib comdlg32.lib dwmapi.lib uxtheme.lib ^
    shell32.lib shlwapi.lib ole32.lib uuid.lib advapi32.lib pdh.lib iphlpapi.lib ws2_32.lib ^
    wtsapi32.lib setupapi.lib dxgi.lib version.lib powrprof.lib crypt32.lib psapi.lib

if errorlevel 1 exit /b 1

> res\build_number.txt (echo !NEXT_BUILD!)

echo.
echo Built single standalone executable: Taskmgr.exe (!BUILD_TAG!)
for %%A in (Taskmgr.exe) do echo Size: %%~zA bytes
