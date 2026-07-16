# 百度API代理服务器

## 为什么需要这个代理？

1. **ESP32的HTTPS被阻止**：移动热点阻止了ESP32直接连接百度API
2. **浏览器的CORS限制**：网页端无法直接调用百度API（跨域问题）

## 解决方案

使用本地代理服务器作为中转：
```
ESP32 --MQTT--> 网页端 --HTTP--> 代理服务器 --HTTPS--> 百度API
```

## 启动方法

### Windows
双击运行 `start_proxy.bat`

### 手动启动
```bash
cd server
pip install flask flask-cors requests
python proxy_server.py
```

## 接口说明

| 接口 | 方法 | 说明 |
|------|------|------|
| `POST /api/tts` | 文本转语音 | 接收JSON `{"text": "要合成的文本"}`，返回WAV音频 |
| `POST /api/asr` | 语音识别 | 接收PCM音频数据，返回识别文本 |
| `GET /health` | 健康检查 | 返回服务器状态 |

## 使用流程

1. 启动代理服务器 (`python proxy_server.py`)
2. 打开网页端 (`index.html`)
3. ESP32通过MQTT发送TTS请求
4. 网页端调用本地代理完成TTS合成
5. 音频通过MQTT返回给ESP32播放

## 依赖

- Python 3.7+
- Flask
- Flask-CORS
- Requests

## 注意

- 代理服务器默认运行在 `http://localhost:8080`
- 确保防火墙允许8080端口
- 代理服务器会自动缓存百度Token，避免频繁请求
