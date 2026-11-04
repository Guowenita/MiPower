@echo off
rem ================================================================
rem  TrafficMonitor 离线模式 - 状态查询
rem
rem  只读查询，不修改任何规则
rem  注意: 读取防火墙规则需要管理员权限，故同样会弹 UAC
rem ================================================================
chcp 936 >nul
setlocal

set "DIR=%~dp0"
set "PS1=%DIR%TrafficMonitor_Offline.ps1"

net session >nul 2>&1
if not "%errorlevel%"=="0" (
    echo [提示] 正在申请管理员权限（读取防火墙规则需要），请在 UAC 窗口点击"是"...
    powershell -NoProfile -ExecutionPolicy Bypass -Command "Start-Process -FilePath '%~f0' -Verb RunAs -WorkingDirectory '%DIR%'" 2>nul
    if not "%errorlevel%"=="0" (
        echo [错误] 提权失败。请右键本文件，选择"以管理员身份运行"
        pause
        exit /b 1
    )
    exit /b 0
)

if not exist "%PS1%" (
    echo [错误] 缺少依赖文件 TrafficMonitor_Offline.ps1
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%PS1%" -Mode status
echo.
pause
endlocal
