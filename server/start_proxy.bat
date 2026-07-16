@echo off
chcp 65001
echo ==========================================
echo 百度API代理服务器 + 前端大屏 启动脚本
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
echo 启动代理服务器（含前端大屏）...
echo.
echo  大屏地址: http://localhost:8090
echo  局域网:   http://192.168.x.x:8090
echo.

:: 延迟 2 秒后自动打开浏览器
start /b timeout /t 2 /nobreak >nul && start http://localhost:8090

cd /d "%~dp0"
python proxy_server.py

pause
