@echo off
:: DuckDNS IP 自动更新脚本
:: 每 5 分钟更新一次盲杖域名

set TOKEN=6ce978ed-6ab9-4381-8b53-1a42eee9e32d
set DOMAIN=blindstick

:: 获取当前公网 IP
for /f "delims=" %%i in ('curl -s ifconfig.me') do set MYIP=%%i

if "%MYIP%"=="" (
    echo %date% %time% 获取IP失败 >> duckdns_update.log
    exit /b 1
)

:: 调用 DuckDNS API 更新
curl -s "https://www.duckdns.org/update?domains=%DOMAIN%&token=%TOKEN%&ip=%MYIP%" >nul

echo %date% %time% 更新 %DOMAIN%.duckdns.org -> %MYIP% >> duckdns_update.log
