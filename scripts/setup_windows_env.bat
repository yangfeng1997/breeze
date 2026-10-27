@echo off
rem 双击运行：绕过 PowerShell 执行策略，调用环境检测/安装脚本
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup_windows_env.ps1" %*
echo.
pause
