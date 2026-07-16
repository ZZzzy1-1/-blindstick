@echo off
chcp 65001
echo ==========================================
echo 百度API代理服务器启动脚本
echo ==========================================
echo.
echo 正在检查Python...
python --version 2>NUL
if errorlevel 1 (
    echo [错误] 未找到Python，请先安装Python 3
    pause
    exit /b 1
)

echo.
echo 正在安装依赖...
pip install flask flask-cors requests -q

echo.
echo 启动代理服务器...
echo 访问地址: http://localhost:8080
echo.
cd /d "%~dp0"
python proxy_server.py

pause
