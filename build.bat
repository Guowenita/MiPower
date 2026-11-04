@echo off
REM ============================================================
REM  MiPower 插件构建脚本
REM  用法: build.bat [debug|release]
REM ============================================================
setlocal enabledelayedexpansion

set CFG=%1
if "%CFG%"=="" set CFG=release

set ROOT=%~dp0
set SRC=%ROOT%src
set OUT=%ROOT%build

REM ---- 定位 MSVC ----
set VCVARS=
for %%V in (18 17) do (
    if not defined VCVARS (
        for %%D in (
            "%ProgramFiles%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
            "%ProgramFiles%\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
            "%ProgramFiles%\Microsoft Visual Studio\2022\Professional\VC\Auxiliary\Build\vcvars64.bat"
            "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
            "%ProgramFiles(x86)%\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
        ) do (
            if exist %%D set VCVARS=%%D
        )
    )
)

if not defined VCVARS (
    echo [ERROR] 未找到 MSVC (vcvars64.bat)
    echo 请安装 Visual Studio Build Tools with C++ workload.
    exit /b 1
)

echo [INFO] 使用 %VCVARS%
call "%VCVARS%" >nul
if errorlevel 1 (
    echo [ERROR] vcvars64 执行失败
    exit /b 1
)

if not exist "%OUT%" mkdir "%OUT%"

set CXXFLAGS=/nologo /std:c++17 /W3 /EHsc /utf-8 /DUNICODE /D_UNICODE ^
 /DWIN32_LEAN_AND_MEAN /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS
set LINKFLAGS=/nologo

if /i "%CFG%"=="debug" (
    set CXXFLAGS=!CXXFLAGS! /Od /Zi /MTd /D_DEBUG
    set LINKFLAGS=!LINKFLAGS! /DEBUG
) else (
    set CXXFLAGS=!CXXFLAGS! /O2 /MT /DNDEBUG
)

REM ---- 插件 DLL ----
echo [INFO] 编译 MiPower.dll (%CFG%)...
cl %CXXFLAGS% /LD /I"%SRC%" ^
    "%SRC%\Crypto.cpp" ^
    "%SRC%\MiioClient.cpp" ^
    "%SRC%\MiotSpec.cpp" ^
    "%SRC%\Config.cpp" ^
    "%SRC%\Aggregator.cpp" ^
    "%SRC%\Collector.cpp" ^
    "%SRC%\MiPowerPlugin.cpp" ^
    "%SRC%\OptionsDlg.cpp" ^
    "%SRC%\PluginInterface.h" ^
    /Fe:"%OUT%\MiPower.dll" ^
    /link %LINKFLAGS% ws2_32.lib comctl32.lib shell32.lib user32.lib gdi32.lib
if errorlevel 1 (
    echo [ERROR] MiPower.dll 编译失败
    exit /b 1
)

REM ---- 自检程序 ----
echo [INFO] 编译 selftest.exe ...
cl %CXXFLAGS% /I"%SRC%" ^
    "%SRC%\selftest.cpp" ^
    "%SRC%\Crypto.cpp" ^
    /Fe:"%OUT%\selftest.exe"
if errorlevel 1 (
    echo [ERROR] selftest 编译失败
    exit /b 1
)

echo.
echo [OK] 构建完成
echo     DLL : %OUT%\MiPower.dll
echo     TEST: %OUT%\selftest.exe
exit /b 0
