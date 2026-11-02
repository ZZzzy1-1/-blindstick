# 智能导盲杖随行助手

基于 **ESP32-S3 + K230** 异构协同的智能导盲辅助系统。系统利用激光雷达实现本地实时避障，利用 K230 端侧视觉识别环境目标，并借助云端服务完成语音识别、语音合成与步行导航，同时提供浏览器端可视化大屏。

系统整体采用“端—云协同”思路：**雷达—避障—电机闭环完全运行在本地**，网络中断时避障功能不降级；语音识别、语音合成与在线导航等能力由云端提供。

---

## 功能特点

- **视觉目标检测**：K230 端侧运行 YOLOv8n，识别盲道、斑马线、立柱、行人、车辆、楼梯、水坑等 14 类目标。
- **动态避障**：激光雷达三向测距，基于左右可通行空间比较选择辅助转向方向，驱动单转向电机。
- **GPS 定位**：解析 NMEA 数据，获取经纬度、速度与卫星数，用于里程统计与导航起点。
- **语音交互**：语音识别（百度 ASR）与语音合成（百度 TTS），音频经 MQTT 分片传输。
- **语音导航**：识别“带我去 / 我要去 / 我想去 / 导航到 / 我去”等触发词，提取目的地并请求步行路线，逐步播报。
- **可视化大屏**：实时展示设备状态、视觉检测框、雷达距离、地图轨迹、事件与导航记录。

---

## 系统架构

```
浏览器大屏 (index.html / app.js)
        │  MQTT over WebSocket (8084)
        ▼
   EMQX Cloud  MQTT Broker (8883)
     │                    │
     ▼                    ▼
 ESP32-S3            云端后端 proxy_server.py
 雷达/避障/电机        Flask + 百度 ASR/TTS
 GPS/音频/MQTT         百度地图 地点/步行路线
     │
     ├── UART2 ── GPS (ATGM336H)
     └── Wi-Fi ── K230 (TCP 8899 / UDP 发现 8898)
```

| 组成 | 职责 |
|---|---|
| **K230** | 运行 YOLOv8n，完成图像采集与目标检测，通过 Wi-Fi 上报检测结果 |
| **ESP32-S3** | FreeRTOS 多任务：雷达解析、避障决策、电机控制、GPS 解析、音频采集/播放、MQTT 通信 |
| **云端后端** | 语音识别、语音合成、地点搜索、步行路线获取，并通过 MQTT 与设备交互 |
| **浏览器端** | 直接订阅 MQTT 数据，完成可视化展示 |

---

## 目录结构

```
.
├─ index.html                    # 可视化大屏
├─ css/
│  ├─ style.css                  # 主样式
│  └─ leaflet.css                # 地图样式
├─ js/
│  ├─ app.js                     # 前端逻辑（MQTT / 地图 / UI）
│  ├─ echarts.min.js             # 图表库
│  ├─ leaflet.js                 # 地图库
│  └─ mqtt.min.js                # MQTT over WebSocket
└─ server/
   ├─ proxy_server.py            # 云端后端（Flask，同时托管前端）
   ├─ main.py                    # K230 视觉程序（CanMV MicroPython）
   ├─ requirements.txt           # 后端依赖
   ├─ .gitignore
   └─ esp32_upload/
      └─ esp32_upload.ino        # ESP32-S3 固件（Arduino）
```

---

## 硬件连接

| 链路 | 接口 / 引脚 | 说明 |
|---|---|---|
| 激光雷达 | UART1 / GPIO18 | YDLIDAR X2L，115200 |
| GPS | UART2 / RX=GPIO17，TX=GPIO16 | ATGM336H，默认 9600，带波特率自动探测 |
| K230 视觉 | Wi-Fi / TCP 8899、UDP 发现 8898 | ESP32 作 TCP 服务端，K230 作客户端 |
| 麦克风 | I2S / BCLK=1，WS=2，SD=42 | INMP441，16 kHz |
| 扬声器 | I2S / BCLK=4，LRCK=5，DIN=6 | MAX98357 |
| 电机驱动 | TB6612 / IN1=12，IN2=11，PWM=10，STBY=9 | 单转向电机 JGA25-370 |
| 云端 | Wi-Fi / MQTT TLS 8883 | EMQX Cloud |

雷达扇区：前方 330°~30°，左侧 60°~120°，右侧 240°~300°；距离采用 EMA 平滑（α=0.5）。

---

## 通信协议

### K230 → ESP32（Wi-Fi TCP，文本帧）

```
DETS:类别,置信度,x,y,w,h;类别,置信度,x,y,w,h;#CRC\n    多目标
DET:类别#CRC\n                                         单目标（兼容）
NONE#CRC\n                                             无目标
```

坐标基于 320×320 检测图；`#CRC` 为字节累加和校验。ESP32 启动 UDP 发现服务（8898），K230 广播探测后自动获取其 IP，无需写死地址。

### MQTT 主题

| 主题 | 方向 | 用途 |
|---|---|---|
| `blindstick/sensors` | 设备 → 网页 | 传感器与设备状态 |
| `blindstick/voice/result` | 设备 → 网页 | 识别文本 |
| `blindstick/tts/request` | 设备 / 网页 → 后端 | 语音合成请求 |
| `blindstick/tts/control` | 后端 → 设备 | 流式播放开始 / 结束 / 打断 |
| `blindstick/tts/stream/<i>` | 后端 → 设备 | TTS 音频分片 |
| `blindstick/asr/control` | 设备 → 后端 | 语音识别会话开始 / 结束 |
| `blindstick/asr/stream/<i>` | 设备 → 后端 | 录音 PCM 分片 |
| `blindstick/asr/result` | 后端 → 设备 | 识别结果文本 |
| `blindstick/nav/steps` | 网页 → 设备 | 导航步骤 |
| `blindstick/config/home_city` | 网页 → 设备 | 常住地设置 |
| `blindstick/stats/detour` | 网页 → 设备 | 路线调整统计 |

设备通过 TLS 8883 接入，网页通过 WebSocket 8084 接入。

---

## 快速开始

### 1. 云端后端

```bash
cd server
pip install -r requirements.txt
python proxy_server.py
```

默认监听 8090 端口（可用环境变量 `PORT` 覆盖）。后端同时托管前端页面，访问 `http://localhost:8090` 即可打开大屏。

### 2. ESP32-S3 固件

1. 用 Arduino IDE 打开 `server/esp32_upload/esp32_upload.ino`；
2. 板卡选择 **ESP32S3 Dev Module**；
3. 工具设置建议：Flash Mode = **QIO 80MHz**，PSRAM = **OPI PSRAM**；
4. 修改文件顶部的 Wi-Fi 名称与密码，编译并烧录。

### 3. K230 视觉程序

1. 将 `best.kmodel` 放入 SD 卡 `/sdcard/examples/kmodel/`；
2. 将 `server/main.py` 拷贝到 SD 卡根目录（开机自动运行）；
3. 修改 `main.py` 顶部的 Wi-Fi 名称与密码，确保与 ESP32 处于同一 2.4G 热点；
4. 上电后自动发现 ESP32 并建立连接。

### 4. 前端

由后端直接托管，打开后端地址即可，无需单独启动。

---

## 配置项

| 配置 | 位置 |
|---|---|
| Wi-Fi SSID / 密码 | `esp32_upload.ino`、`main.py` 顶部 |
| MQTT Broker / 账号 / 密码 | `esp32_upload.ino`、`proxy_server.py`、`js/app.js` |
| 百度语音 API Key / Secret | `proxy_server.py`、`js/app.js` |
| 百度地图 AK | `proxy_server.py`、`js/app.js` |
| 常住地 | 网页右上角设置，保存于 localStorage 并下发设备 |

> 注意：以上密钥与密码目前在源码中以明文形式配置，部署或公开前请替换为自己的凭据。

---

## 技术栈

- **边缘视觉**：K230、CanMV（MicroPython）、YOLOv8n
- **边缘控制**：ESP32-S3、Arduino、FreeRTOS
- **云端服务**：Python、Flask、百度语音识别 / 合成、百度地图 API、EMQX Cloud
- **前端**：HTML / CSS / JavaScript、Leaflet.js、ECharts、MQTT.js
