#!/usr/bin/env python3
"""一键获取 ngrok 公网地址并更新 _redirects"""
import json, urllib.request, os, sys

try:
    with urllib.request.urlopen("http://localhost:4040/api/tunnels", timeout=3) as r:
        data = json.loads(r.read())

    public_url = data["tunnels"][0]["public_url"]
    print(f"[ngrok] 当前地址: {public_url}")

    redirects_path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "_redirects")
    content = f"/api/*  {public_url}/:splat  200\n"

    with open(redirects_path, "w", encoding="utf-8") as f:
        f.write(content)

    print(f"[_redirects] 已更新:")
    print(content)
    print(">>> 请执行: git add _redirects && git commit -m 'Update ngrok' && git push")

except Exception as e:
    print(f"[错误] {e}")
    print("请确认 ngrok 正在运行: ngrok http 8080")
    sys.exit(1)
