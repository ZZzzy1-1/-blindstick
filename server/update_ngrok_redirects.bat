@echo off
chcp 65001 >nul 2>&1
echo === 获取当前 ngrok 地址 ===

:: 从 ngrok API 获取公网地址
for /f "delims=" %%i in ('curl -s http://localhost:4040/api/tunnels ^| findstr "public_url"') do (
    set "line=%%i"
)

:: 提取 URL（从 "public_url":"https://xxx" 中提取）
for /f "tokens=2 delims=:" %%u in ('echo %line% ^| findstr "https"') do (
    set "raw=%%u"
)

:: 去掉引号和空格
set "url=%raw:"=%"
set "url=%url: =%"

if "%url%"=="" (
    echo 未找到 ngrok 隧道，请确认 ngrok 正在运行
    pause
    exit /b 1
)

echo 当前地址: %url%
echo.

:: 更新 _redirects 文件
set "REDIRECTS=%~dp0_redirects"
echo /api/*  %url%/:splat  200 > "%REDIRECTS%"

echo _redirects 已更新:
type "%REDIRECTS%"
echo.
echo 记得 git add _commit _push 到 Netlify
pause
