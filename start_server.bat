@echo off
echo ========================================
echo   导盲杖系统 - 启动代理服务器
echo ========================================
cd /d "%~dp0\server"
python proxy_server.py
pause
