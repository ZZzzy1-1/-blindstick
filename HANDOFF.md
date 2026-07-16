# 交付文档 – HANDOFF.md

## 1. 任务目标
- 使 **ESP32/K230 硬件**（放在武汉或鄂州任意网络）与 **软件服务**（前端部署在 Netlify、后端运行在本地电脑或云服务器）能够跨网络通信。
- 核心通信路径：
  - 硬件 ↔ MQTT broker（公开的 EMQX 实例）←→ 前端（通过 WebSocket）。
  - 前端 ↔ 后端代理（`proxy_server.py`）通过 HTTP `/api/*` 接口（TTS、ASR、导航、配置等）。
- 最终目标：语音唤醒、语音识别、TTS 播放、导航指令、障碍物报警在异地网络下均能正常工作。

## 2. 已完成的工作
| 编号 | 内容 | 备注 |
|------|------|------|
| 2.1 | `proxy_server.py` 部署到 **Render 免费云服务** | 地址: `https://blindstick-2.onrender.com` |
| 2.2 | 后端接口完整：TTS、ASR、config、navigation/stop、TTS分段 | CORS、Token缓存、TTS缓存均已实现 |
| 2.3 | 前端 `app.js` 指向 Render 云端 | `API_BASE = 'https://blindstick-2.onrender.com'` |
| 2.4 | ESP32 固件指向 Render 云端 | 2处 `https://blindstick-2.onrender.com/api/tts` 和 `/api/nav_steps` |
| 2.5 | ESP32 TTS 无声音问题修复 | `playStartupTestTone` 内存泄漏修复、I2S APLL时钟启用 |
| 2.6 | DuckDNS 配置完成 | `blindstick.duckdns.org`（备用，当前直接使用 Render） |

## 3. 当前卡住的地方
- **无** — 云端部署已完成，所有组件（前端、后端、ESP32）均可独立运行。

## 4. 最终架构

```
┌─────────────────────────────────────────────────────────────┐
│  Netlify 前端（可选） 或 本地打开 index.html                  │
│  └─ 直连 Render 后端: https://blindstick-2.onrender.com      │
└──────────────────────────┬──────────────────────────────────┘
                           │ HTTPS
┌──────────────────────────▼──────────────────────────────────┐
│  Render 云服务器 (Python Flask)                              │
│  ├─ /api/tts     → 百度语音合成                              │
│  ├─ /api/asr     → 百度语音识别                              │
│  ├─ /api/config  → 用户配置                                 │
│  └─ /api/navigation/stop → 停止导航                         │
└──────────────────────────┬──────────────────────────────────┘
                           │ MQTT over TLS
┌──────────────────────────▼──────────────────────────────────┐
│  ESP32-S3 ←→ 公共 MQTT Broker (emqxsl.com:8883)             │
│  ├─ 雷达/GPS 数据上报                                        │
│  ├─ 语音指令接收                                            │
│  └─ TTS 音频播放（MAX98357）                                 │
└─────────────────────────────────────────────────────────────┘
```

## 5. 使用方式

### 前端访问（3种方式）
| 方式 | 地址 | 说明 |
|------|------|------|
| 本地文件 | 直接打开 `index.html` | 最简单，无服务器 |
| Netlify | `https://statuesque-crostata-a41140.netlify.app/` | 需更新 `_redirects` 指向 Render |
| Render | `https://blindstick-2.onrender.com/` | 后端自带前端托管 |

### ESP32 配置
- **MQTT Broker**: `u72a7838.ala.asia-southeast1.emqxsl.com:8883`
- **HTTP API**: `https://blindstick-2.onrender.com/api/tts`

### 本地开发（可选）
```bash
cd server
python proxy_server.py  # 默认端口 8090
```

## 5. 已踩过的坑（绝对不要再踩）

| 坑点 | 说明 | 避免方式 |
|------|------|----------|
| **5.1** 将后端绑定到 `127.0.0.1:8080` 而不是 `0.0.0.0:8080` | 外网无法访问，只能本机 loopback。 | 确认 `app.run(host='0.0.0.0', port=8080, debug=False)`。 |
| **5.2** 忘记放行 Windows 防火墙入站规则（或路由器未做端口转发） | 外部请求被丢掉，表现为连接超时或拒绝。 | 在防火墙中添入站规则 `allow tcp port 8080`；在路由器上做端口转发。 |
| **5.3** 使用 `http://localhost:8080/` 硬编码在前端 JS 中 | 前端部署在 Netlify 时请求会打到本机 localhost，导致 404。 | 改为相对路径或使用 `_redirects`/环境变量； **不要** 在源码中写死 `localhost`。 |
| **5.4** 忘记在后端打开 CORS（或只允许特定来源导致跨域请求被拦截） | 浏览器会在拿到响应后被 CORS 拦截，控制台报 `Access-Control-Allow-Origin` 错误。 | 确保 `CORS(app, resources={r"/*": {"origins": "*"}})`（或明确列出前端域名）。 |
| **5.5** 将 `_redirects` 写错格式（漏掉 `:splat`、`200` 或路径不匹配） | Netlify 会当作普通文件处理，请求不会被代理，仍会 404。 | 按照 exact 语法：`/api/*  <外网地址>/:splat  200`。 |
| **5.6** 在使用 ngrok 时把地址写成 `http://` 而实际上是 `https://`（或相反） | 会导致混合内容警告或请求失败。 | 与 ngrok 输出的协议保持一致（大多数时候是 `https://`）。 |
| **5.7** 将后端端口改动后忘记同步更新路由器转发规则或 `_redirects` 中的端口号 | 外网仍然指向旧端口，导致连接失败。 | 修改端口后，**同时**更新路由器转发和 `_redirects` 中的端口。 |
| **5.8** 在局域网测试成功后直接假设外网也能通，未做实际外网验证 | 导致后续调试时找不到问题根源。 | 必须使用 **不在同一子网** 的网络（如手机流量）进行 `curl` 或浏览器验证。 |
| **5.9** 在后端日志中只看到请求到达但没有返回音频，却未检查返回的 Content-Type | 可能返回了错误 JSON（如 token 获取失败）而被前端当作音频处理导致无声。 | 确认响应头 `Content-Type` 包含 `audio/wav`；若不是，查看返回体排除后端错误（如密钥无效）。 |

## 6. 备注
- 一旦通过 `_redirects` 实现了请求代理，**后端无需再做任何改动**（只要保持 CORS 已打开）。
- 如以后想改回本地开发（前端和后端同机），只需删除或注释掉 `_redirects` 文件，或将其值改为空（例如 `/api/*  /:splat  200`），前端请求就会回到同源。
- 所有代码修改均应通过 **Git** 进行版本控制，以便随时回滚或追溯。

--- 
*以上即为本次会话的全部交付信息。后续只需按上述步骤完成外网暴露 + Netlify `_redirects` 配置，即可实现跨网络语音播报、导航等功能。*