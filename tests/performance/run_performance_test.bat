@echo off
chcp 65001 >nul
cd /d "%~dp0"

echo 启动个人云备份服务器自动化性能测试...
echo.
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0performance_test.ps1"

echo.
pause
