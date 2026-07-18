# 导盲杖随行助手 - 完整场景实现逻辑复现

## 📱 场景一：系统启动

### 时序图
```
时间轴 →

[ESP32上电]
    │
    ▼
┌─────────────┐
│  播放本地提示音  │ ← playLocalStartupTone() (1000Hz测试音0.5秒)
│  (确认扬声器正常) │
└─────────────┘
    │
    ▼
┌─────────────┐
│  初始化WiFi    │ ← WiFi.begin()
│  连接WiFi     │
└─────────────┘
    │
    ▼
┌─────────────┐
│  初始化MQTT   │ ← mqtt.setServer()
│  设置TLS     │
└─────────────┘
    │
    ▼
┌─────────────┐
│  连接MQTT    │ ← mqtt.connect()
│  订阅主题    │
└─────────────┘
    │
    ▼
[MQTT连接成功回调]
    │
    ▼
┌─────────────────┐
│  首次连接?        │ ← if (!startup_announced && !startup_announced_rtc)
│  是 → 播放开机语音  │ ← playStartupVoice() (本地音频startup_audio[])
│  设置标志为已播放   │ ← startup_announced = true
└─────────────────┘
    │
    ▼
[创建FreeRTOS任务]
    ├── Core 0: RadarMotorUploadTask (雷达+电机+数据上传)
    ├── Core 1: NavigationTask (导航播报)
    └── Core 1: VoiceRecognitionTask (语音识别)
```

### 关键代码链路
```cpp
// 1. setup() 初始化
void setup() {
    // ... 初始化硬件 ...
    
    // 2. 播放测试音
    playLocalStartupTone();
    
    // 3. 连接WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    
    // 4. 连接MQTT
    mqtt_reconnect();  // 连接成功后会播放开机语音
    
    // 5. 创建任务
    xTaskCreatePinnedToCore(RadarMotorUploadTask, "RadarTask", 8192, NULL, 3, &RadarTaskHandle, 0);
    xTaskCreatePinnedToCore(NavigationTask, "NavTask", 2048, NULL, 1, &NavTaskHandle, 1);
    xTaskCreatePinnedToCore(VoiceRecognitionTask, "VoiceRecTask", 8192, NULL, 2, &VoiceTaskHandle, 1);
}
```

### 前端大屏显示
- 状态栏：主控、雷达、GPS、语音 → 依次变绿
- 地图：显示默认位置（中国中心）
- 事件记录：显示"设备启动成功"

---

## 🎯 场景二：障碍物检测与告警

### 触发条件
雷达检测到前方/侧方距离小于阈值：
- 正前方 < 80cm
- 左前方 < 80cm  
- 右前方 < 80cm
- 左侧 < 40cm
- 右侧 < 40cm

### 完整链路

```
[YDLIDAR X2雷达扫描]
    │ 持续360°扫描，UART串口数据
    ▼
[ESP32 - Serial1接收]
    │ processRadarPacket() 解析雷达协议
    ▼
[五向雷达数据平滑]
    │ EMA平滑算法: dir_smt[i] = SMOOTH_A * dir_raw[i] + (1-SMOOTH_A) * dir_smt[i]
    ▼
[避障决策 smartAvoidDecision()]
    │
    ├── 前方被阻挡? → 电机转向控制
    │   ├── 左转条件: fL > fR && fL > 100cm
    │   └── 右转条件: fR > fL && fR > 100cm
    │
    └── 侧边告警? → 微调电机
        ├── 左侧太近 → 向右微调
        └── 右侧太近 → 向左微调
    ▼
[障碍物检测 checkObstacleAndAlert()]
    │
    ├── 检查条件:
    │   ├── 是否正在播放语音? is_ai_talking
    │   ├── 是否正在请求TTS? getTTSRequesting()
    │   ├── 距离上次播报是否>8秒?
    │   └── 相同文本是否>8秒?
    │
    └── 条件通过 → 生成告警文本
        ├── "前方有障碍物，请向左绕行"
        ├── "左前方有障碍物，请向右绕行"
        └── ... (根据方位智能选择)
    ▼
[TTS请求发布]
    │ mqtt.publish("blindstick/tts/request", {"text": "...", "priority": 2})
    ▼
[MQTT Broker中转]
    ▼
[代理服务器接收]
    │ on_message() → 新线程handle_tts_request()
    ▼
[百度TTS合成]
    │ POST https://tsn.baidu.com/text2audio
    │ 参数: tex, tok, spd=5, pit=5, vol=9, per=1
    ▼
[保存音频文件]
    │ audio_cache/tts_{timestamp}_{hash}.wav
    ▼
[生成URL并推送]
    │ mqtt.publish("blindstick/tts/url", {"url": "https://...", "text": "..."})
    ▼
[MQTT Broker中转]
    ▼
[ESP32接收 handleTTSUrl()]
    │
    ├── 1. 暂停语音识别任务 vTaskSuspend(VoiceTaskHandle)
    ├── 2. HTTP下载音频 (最多15秒超时)
    ├── 3. 跳过WAV头44字节
    ├── 4. I2S播放音频 MAX98357功放
    ├── 5. 等待播放完成 delay(duration_ms + 100)
    ├── 6. 重置TTS标志 setTTSRequesting(false)
    └── 7. 恢复语音识别 vTaskResume(VoiceTaskHandle)
    ▼
[电机控制]
    └── 根据避障决策控制TB6612电机驱动
```

### 前端大屏实时更新
```javascript
// 雷达数据可视化
updateRadarCircles(front, frontLeft, frontRight, left, right) {
    // 更新进度条颜色和宽度
    bar.style.width = (distance / 400) * 100 + '%';
    if (distance < 180) bar.classList.add('danger');  // 红色
    else if (distance < 300) bar.classList.add('warn'); // 黄色
    else bar.classList.add('safe'); // 绿色
}

// 障碍物事件记录
addEventLog('障碍物', `检测到障碍物，距离 ${distance}cm`);

// 统计数据
AppState.reportData.obstacleCount++;
document.getElementById('obstacleCount').textContent = obstacleCount;
```

---

## 🗣️ 场景三：语音导航（用户说"带我去天安门"）

### 完整链路

```
[用户按下录音按钮/语音唤醒]
    ▼
[ESP32 - VoiceRecognitionTask]
    │
    ├── 等待开机语音播放完成
    ├── 暂停时机: !is_ai_talking && !getTTSRequesting()
    └── 开始录音 5秒钟
    ▼
[I2S麦克风采集 PCM数据]
    │ INMP441 → ESP32 I2S_NUM_0
    │ 采样率: 16kHz, 16bit, 单声道
    ▼
[Base64编码]
    │ base64Encode(pcm_buffer, pcm_length)
    ▼
[百度ASR语音识别]
    │ POST https://vop.baidu.com/server_api
    │ 参数: speech(base64), format=pcm, rate=16000
    ▼
[识别结果返回]
    │ 假设: "带我去天安门"
    ▼
[ESP32提取目的地]
    │ extractDestination("带我去天安门")
    │
    ├── 查找触发词: "带我去"
    ├── 提取触发词后内容: "天安门"
    ├── 过滤非目的地词汇
    └── 返回: "天安门"
    ▼
[发送目的地到前端]
    │ mqtt.publish("blindstick/nav/steps", {destination: "天安门"})
    ▼
[前端大屏处理]
    │ app.js - handleVoiceNavigationAdvanced("天安门")
    │
    ├── 搜索最近目的地
    │   └── 百度地点检索API
    │       GET https://api.map.baidu.com/place/v2/search
    │       参数: query=天安门, region=黄石市, page_size=10
    │
    ├── 计算各结果与当前位置距离
    │   └── calcDistance(lat1, lng1, lat2, lng2) - Haversine公式
    │
    └── 选择最近的结果
        └── 天安门广场: 距离 5.2km
    ▼
[百度步行路线规划]
    │ GET https://api.map.baidu.com/directionlite/v1/walking
    │ 参数: origin={lat},{lng}, destination={lat},{lng}
    ▼
[前端发送导航路线到ESP32]
    │ mqtt.publish("blindstick/nav/steps", {
    │   status: "ok",
    │   destination: "天安门",
    │   steps: ["向东出发", "左转进入...", ...],
    │   distance: 5200,
    │   duration: 3900
    │ })
    ▼
[ESP32接收导航路线]
    │ mqtt_callback() → topic == MQTT_TOPIC_NAV_STEPS
    │
    ├── 解析steps数组
    ├── nav_steps[0] = "向东出发"
    ├── nav_steps[1] = "左转进入..."
    ├── nav_total_steps = 5
    ├── current_step_idx = 0
    └── nav_active = true
    ▼
[TTS播报导航开始]
    │ "开始导航到天安门，全程5200米，预计65分钟，向东出发"
    ▼
[前端大屏更新]
    │
    ├── updateNavigationSteps(steps)
    ├── 显示步骤列表
    ├── 当前步骤高亮
    ├── 进度条: (current_step + 1) / total_steps * 100%
    └── addNavHistory("天安门", steps)
```

### NavigationTask 导航播报逻辑
```cpp
void NavigationTask(void* pvParameters) {
    while (true) {
        if (nav_active && current_step_idx < nav_total_steps) {
            
            // 检测是否进入新路段
            if (current_step_idx != last_step_idx) {
                last_step_idx = current_step_idx;
                
                // 生成播报文本
                if (current_step_idx == 0) {
                    // 第一步已在planWalkingRoute播报，跳过
                } else if (current_step_idx >= total - 1) {
                    announcement = "即将到达目的地，" + nav_steps[current_step_idx];
                } else {
                    announcement = "下一个路口，" + nav_steps[current_step_idx];
                }
                
                // TTS播报
                mqtt.publish("blindstick/tts/request", 
                    {"text": announcement, "priority": PRIO_NORMAL});
            }
            
            // 模拟导航进度 (每3秒增加5%)
            if (!is_blocked && !is_ai_talking) {
                vTaskDelay(3000ms);
                current_progress += 5;
            }
            
            // 进度完成，进入下一步
            if (current_progress >= 100) {
                current_progress = 0;
                current_step_idx++;
                
                if (current_step_idx >= nav_total_steps) {
                    // 导航完成
                    nav_active = false;
                    mqtt.publish("blindstick/tts/request", 
                        {"text": "导航完成，已到达目的地", "priority": PRIO_NORMAL});
                }
            }
        }
        vTaskDelay(50ms);
    }
}
```

---

## 📊 场景四：传感器数据实时上传

### 数据流向

```
[RadarMotorUploadTask - Core 0]
    │
    ├── 循环读取雷达数据 (每15ms)
    ├── 解析GPS NMEA数据
    ├── 更新五向雷达平滑数据
    ├── 执行避障决策
    │
    └── 每200ms执行一次:
        └── publishSensorData()
            ▼
        [组装JSON数据]
            {
              "device_id": "blind_stick_001",
              "radar": {
                "f": 120.5,    // 正前
                "fl": 350.0,   // 前左
                "fr": 400.0,   // 前右
                "l": 380.0,    // 左侧
                "r": 200.0     // 右侧
              },
              "blocked": true,
              "nav": true,
              "gps": {
                "lat": 30.23193,
                "lng": 115.05791,
                "sats": 8
              }
            }
            ▼
        [MQTT发布]
            mqtt.publish("blindstick/sensors", json_buffer)
            ▼
        [MQTT Broker]
            ▼
        [前端接收]
            app.js - handleMqttMessage()
            ▼
        [前端处理]
            ├── updateRadarCircles(f, fl, fr, l, r)
            ├── updateGPS(lng, lat, speed)
            ├── updateSatellites(sats)
            ├── updateModuleStatus({radar: true, gps: sats > 0})
            └── updateRealtimeNav({nav_destination, nav_step, ...})
```

### GPS轨迹记录
```javascript
// 前端地图轨迹
let gpsHistory = [];

function updateGPS(lng, lat, speed) {
    // 添加到轨迹
    gpsHistory.push([lat, lng]);
    
    // 计算里程
    if (lastGpsPos) {
        const dist = calcDistance(lastGpsPos.lat, lastGpsPos.lng, lat, lng);
        if (dist > 1 && dist < 100) { // 过滤跳变
            AppState.reportData.totalMileage += dist;
        }
    }
    lastGpsPos = {lat, lng};
    
    // 更新地图
    pathPolyline.addLatLng([lat, lng]);
    marker.setLatLng([lat, lng]);
    map.panTo([lat, lng]);
}
```

---

## 🔄 场景五：常住地设置

### 用户操作
1. 点击前端右上角 ⚙️ 设置按钮
2. 输入常住地城市（如"北京市"）
3. 点击保存

### 完整链路

```
[前端 - saveSettings()]
    │
    ├── localStorage.setItem('homeCity', '北京市')
    ├── API_CONFIG.homeCity = '北京市'
    │
    └── mqtt.publish("blindstick/config/home_city", {
            type: 'home_city_update',
            city: '北京市'
        })
    ▼
[MQTT Broker]
    ▼
[ESP32接收]
    │ mqtt_callback() → topic == "blindstick/config/home_city"
    │
    ├── 解析JSON: doc["city"] = "北京市"
    ├── home_city = "北京市"
    │
    └── TTS播报确认
        mqtt.publish("blindstick/tts/request", {
            "text": "常住地已设置为北京市",
            "priority": PRIO_NORMAL
        })
    ▼
[前端显示确认]
    showToast('常住地已设置为：北京市');
```

---

## 📈 场景六：今日出行统计

### 统计项目
| 项目 | 数据来源 | 更新时机 |
|------|----------|----------|
| 总里程 | GPS轨迹计算 | 每次GPS更新时 |
| 导航次数 | 导航启动计数 | 收到新路线时 |
| 障碍物提醒 | 障碍物检测计数 | checkObstacleAndAlert()触发时 |
| 路线调整 | 重新规划计数 | 收到新路线且已在导航中时 |

### 代码实现
```javascript
// 总里程计算
if (lastGpsPos) {
    const dist = calcDistance(lastGpsPos.lat, lastGpsPos.lng, lat, lng);
    if (dist > 1 && dist < 100) {
        AppState.reportData.totalMileage += dist;
        document.getElementById('totalMileage').textContent = 
            Math.round(AppState.reportData.totalMileage);
    }
}

// 导航次数
function addNavHistory(destination, steps) {
    AppState.reportData.navCount++;
    document.getElementById('navCount').textContent = AppState.reportData.navCount;
}

// 障碍物提醒
function handleObstacleDetection(radarData) {
    if (minDist < OBSTACLE_THRESHOLD && !AppState.lastObstacleState) {
        AppState.reportData.obstacleCount++;
        document.getElementById('obstacleCount').textContent = 
            AppState.reportData.obstacleCount;
    }
}

// 路线调整
if (topic === MQTT_CONFIG.topics.navSteps) {
    if (AppState.reportData.navCount > 0 && !AppState.navJustStarted) {
        AppState.reportData.detourCount++;
        document.getElementById('detourCount').textContent = 
            AppState.reportData.detourCount;
    }
}
```

---

## 🎯 核心设计亮点

### 1. 去中心化架构
- 前端大屏不依赖代理服务器，直接通过MQTT与ESP32通信
- 代理服务器只处理TTS合成（百度API调用）

### 2. 多任务并行
```
Core 0 (高实时性):
├── RadarMotorUploadTask (优先级3) - 雷达+电机+MQTT

Core 1 (业务逻辑):
├── NavigationTask (优先级1) - 导航播报
└── VoiceRecognitionTask (优先级2) - 语音识别
```

### 3. 多重防重复保护
- ESP32: 8秒间隔 + 相同文本8秒去重 + TTS请求标志
- 代理服务器: 开机语音过滤
- 前端: 3秒URL去重

### 4. 优先级TTS系统
- PRIO_HIGH (2): 雷达告警 - 可打断其他播报
- PRIO_NORMAL (1): 导航/对话
- PRIO_LOW (0): 普通导航

### 5. 完整的闭环控制
```
传感器数据采集 → 决策算法 → 执行器控制 → 语音反馈 → 大屏展示
     ↑___________________________________________________|
```
