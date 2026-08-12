/**
 * 导盲杖随行助手 - 网页端主逻辑（MQTT 离线版）
 *
 * 架构：ESP32 → MQTT broker → 网页端（去中心化，不依赖 server.py）
 * Broker: u72a7838.ala.asia-southeast1.emqxsl.com:8084 (WebSocket TLS)
 * Topics:
 *   blindstick/sensors  — 接收 ESP32 传感器数据
 *   blindstick/tts/req  — 向 ESP32 发送 TTS 文本请求（可选）
 *   blindstick/nav/steps — 向 ESP32 发送导航路线
 */

// ================= 14类检测目标定义 =================
const DETECTION_CLASSES = {
    blind_track:       { label: '盲道',       color: '#00d4ff' },
    curb:              { label: '马路牙子',   color: '#7bed9f' },
    crosswalk:         { label: '斑马线',     color: '#ffffff' },
    pole:              { label: '立柱',       color: '#1e90ff' },
    ashcan:            { label: '垃圾桶',     color: '#747d8c' },
    reflective_cone:   { label: '反光锥',     color: '#ffa502' },
    red_light:         { label: '红灯',       color: '#ff4757' },
    yellow_light:      { label: '黄灯',       color: '#ffa502' },
    green_light:       { label: '绿灯',       color: '#2ed573' },
    stop_sign:         { label: '标志牌',     color: '#ff4757' },
    person:            { label: '行人',       color: '#ff4757' },
    vehicle:           { label: '车辆',       color: '#ff6348' },
    stairs:            { label: '楼梯台阶',   color: '#ced6e0' },
    puddle:            { label: '水坑',       color: '#1e90ff' }
};

const CATEGORY_GROUPS = [
    { key: 'blind_track', label: '盲道' },
    { key: 'curb', label: '马路牙子' },
    { key: 'crosswalk', label: '斑马线' },
    { key: 'pole', label: '立柱' },
    { key: 'ashcan', label: '垃圾桶' },
    { key: 'reflective_cone', label: '反光锥' },
    { key: 'red_light', label: '红灯' },
    { key: 'yellow_light', label: '黄灯' },
    { key: 'green_light', label: '绿灯' },
    { key: 'stop_sign', label: '标志牌' },
    { key: 'person', label: '行人' },
    { key: 'vehicle', label: '车辆' },
    { key: 'stairs', label: '楼梯台阶' },
    { key: 'puddle', label: '水坑' }
];

// ================= 全局状态 =================
const AppState = {
    videoDetections: [],
    gpsHistory: [],
    isRunning: true,
    gpsCenter: null,
    gpsInitialized: false,
    gpsHasFix: false,
    reportData: {
        totalMileage: 0, navCount: 0, obstacleCount: 0, detourCount: 0,
    },
    mqttConnected: false,
    imgSize: [320, 320],
    navHistory: [],
    config: { homeCity: '黄石市' },
    lastObstacleState: false,
    lastGpsPos: null,
    navStartTime: null,
    navJustStarted: false,
    voiceSegments: [],
    voiceSegmentCount: 0,
    voiceSegmentReceived: 0,
    // 标记是否已收到过真实的雷达数据
    radarDataReceived: false,
    // 标记是否已收到设备启动事件
    deviceStarted: false
};

// ================= 计算两点间距离（米）====================
function calcDistance(lat1, lng1, lat2, lng2) {
    const R = 6371000;
    const dLat = (lat2 - lat1) * Math.PI / 180;
    const dLng = (lng2 - lng1) * Math.PI / 180;
    const a = Math.sin(dLat/2) * Math.sin(dLat/2) +
              Math.cos(lat1 * Math.PI / 180) * Math.cos(lat2 * Math.PI / 180) *
              Math.sin(dLng/2) * Math.sin(dLng/2);
    const c = 2 * Math.atan2(Math.sqrt(a), Math.sqrt(1-a));
    return R * c;
}

// 后端代理地址（Render 云端）
const API_BASE = 'https://blindstick-4.onrender.com';

// ================= 语音/导航 API 配置 =================
const API_CONFIG = {
    baiduAppId: '123607377',
    baiduApiKey: 'Xbxnhkwb2sxtB6HbH5BUTlUG',
    baiduSecretKey: 'Tw485P2BFGpPu8WeOVP6hy4S1BHqG4ON',
    baiduMapAk: 'e9R2xrzLSwLzjMH5fdqHz4dLB0gXwIZW',
    qwenApiKey: 'sk-57df3af8a02e485ca61469fa10f68c7e',
    homeCity: '黄石市'
};

// ================= 百度实时语音识别 WebSocket 配置 =================
let baiduASRWS = null;
let isRecording = false;
let audioContext = null;
let mediaStream = null;
let audioProcessor = null;
let asrCallback = null;

// ================= 目的地关键词过滤配置 =================
const DESTINATION_FILTER_WORDS = [
    '的', '了', '在', '是', '我', '有', '和', '就', '不', '人', '都', '一', '一个', '上', '也', '很', '到', '说', '要', '去', '你', '会', '着', '没有', '看', '好', '自己', '这', '那', '这些', '那些',
    '一下', '那个', '这个', '那里', '这里', '吧', '啊', '呢', '吗', '哦', '嗯', '唉', '哎', '哈', '呀',
    '请', '把', '给', '跟', '对', '向', '从', '让', '被', '比', '为', '与', '及', '或', '而', '且', '但', '如果', '因为', '所以', '虽然', '然后', '当时', '现在', '今天', '明天', '昨天',
    '附近', '周围', '旁边', '对面', '这里', '那里', '哪儿', '哪里'
];

const NAVIGATION_TRIGGERS = ['带我去', '我要去', '我想去', '导航到', '我去', '去', '到', '往', '走', '来'];

// ================= 最大导航距离（米）====================
const MAX_NAVIGATION_DISTANCE = 10000;

let baiduToken = null;
let baiduTokenExpire = 0;

// --- 获取百度 Access Token ---
async function getBaiduToken() {
    if (baiduToken && Date.now() < baiduTokenExpire - 300000) return baiduToken;
    const url = `https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials&client_id=${API_CONFIG.baiduApiKey}&client_secret=${API_CONFIG.baiduSecretKey}`;
    try {
        const r = await fetch(url, { method: 'POST' });
        const d = await r.json();
        if (d.access_token) {
            baiduToken = d.access_token;
            baiduTokenExpire = Date.now() + (d.expires_in || 2592000) * 1000;
            return baiduToken;
        }
    } catch (e) { console.error('[百度] Token获取失败:', e); }
    return null;
}

// --- 百度 ASR 语音识别 ---
async function baiduASR(pcmBytes) {
    const token = await getBaiduToken();
    if (!token) return null;
    const pcmBase64 = btoa(String.fromCharCode(...pcmBytes));
    const payload = {
        format: 'pcm', rate: 16000, channel: 1,
        cuid: 'blindstick_web', token: token, dev_pid: 1537,
        speech: pcmBase64, len: pcmBytes.length
    };
    try {
        const r = await fetch('https://vop.baidu.com/server_api', {
            method: 'POST', headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify(payload)
        });
        const d = await r.json();
        if (d.err_no === 0 && d.result && d.result[0]) {
            return d.result[0].trim();
        }
    } catch (e) { console.error('[百度ASR] 失败:', e); }
    return null;
}

// 注意：所有语音播放由ESP32硬件功放完成，前端不播放任何声音

// --- 百度 TTS（请求代理服务器合成，但只发送给ESP32播放，前端不播放）---
async function baiduTTSWeb(text) {
    try {
        const response = await fetch(`${API_BASE}/api/tts`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ text })
        });

        if (response.ok) {
            const audioData = await response.arrayBuffer();
            return new Uint8Array(audioData);
        }
    } catch (e) {
        console.error('[百度TTS] 请求异常:', e.message);
    }
    return null;
}

// --- 百度 TTS（ESP32直接调用，网页端只负责转发音频数据给ESP32）---
async function baiduTTS(text) {
    if (mqttClient && AppState.mqttConnected) {
        const msg = JSON.stringify({
            type: 'tts_request',
            text: text
        });
        mqttClient.publish(MQTT_CONFIG.topics.ttsReq, msg);
    }
    return null;
}

// ==================== 流式TTS（新）====================
const TTS_PRIORITY = {
    LOW: 0,
    NORMAL: 1,
    HIGH: 2
};

let currentTTSSession = {
    sessionId: null,
    priority: 0,
    isPlaying: false
};

async function streamTTS(text, priority = TTS_PRIORITY.NORMAL) {
    if (!mqttClient || !AppState.mqttConnected) {
        console.error('[流式TTS] MQTT未连接');
        return false;
    }

    try {
        const msg = JSON.stringify({
            type: 'tts_request',
            text: text,
            priority: priority,
            ts: Date.now()
        });

        mqttClient.publish('blindstick/tts/request', msg);
        return true;
    } catch (e) {
        console.error('[流式TTS] 发送失败:', e.message);
        return false;
    }
}

async function interruptTTS(newPriority = TTS_PRIORITY.HIGH) {
    if (!mqttClient || !AppState.mqttConnected) return;

    try {
        const msg = JSON.stringify({
            type: 'tts_interrupt',
            priority: newPriority,
            ts: Date.now()
        });
        mqttClient.publish('blindstick/tts/control', msg);
    } catch (e) {
        console.error('[流式TTS] 打断请求失败:', e.message);
    }
}

async function announceNavigation(text) {
    await streamTTS(text, TTS_PRIORITY.LOW);
}

// --- 处理语音导航（简化版 - 调用改进版）---
async function handleVoiceNavigation(pcmBytes) {
    const text = await baiduASR(pcmBytes);
    if (!text) {
        await streamTTS('语音识别失败，请重新说出目的地', TTS_PRIORITY.NORMAL);
        return;
    }
    await handleVoiceNavigationAdvanced(text);
}

// ================= MQTT 配置 =================
const MQTT_CONFIG = {
    host: 'u72a7838.ala.asia-southeast1.emqxsl.com',
    port: 8084,
    path: '/mqtt',
    username: 'blindstick',
    password: '2026',
    clientId: 'blindstick_web_' + Math.random().toString(16).substr(2, 8),
    topics: {
        sensors:   'blindstick/sensors',
        ttsReq:    'blindstick/tts/request',
        ttsAudio:  'blindstick/tts/audio',
        navSteps:  'blindstick/nav/steps',
        voiceSeg:  'blindstick/voice/segments',
        voicePcm0: 'blindstick/voice/pcm/0',
        voicePcm1: 'blindstick/voice/pcm/1',
        voicePcm2: 'blindstick/voice/pcm/2',
        voicePcm3: 'blindstick/voice/pcm/3'
    }
};

let mqttClient = null;
// 【新增】心跳检测相关变量
let lastDataTime = 0;
const HEARTBEAT_INTERVAL = 5000;  // 5秒无数据视为断开
const RECONNECT_INTERVAL = 3000;  // 3秒后重连

// ================= 心跳检测 =================
function startHeartbeatCheck() {
    setInterval(() => {
        if (AppState.mqttConnected) {
            const now = Date.now();
            if (now - lastDataTime > HEARTBEAT_INTERVAL) {
                console.warn('[MQTT] 心跳超时，数据已', (now - lastDataTime) / 1000, '秒未更新');
                // 数据超时，尝试重新订阅
                if (mqttClient && mqttClient.connected) {
                    console.log('[MQTT] 重新订阅传感器主题...');
                    mqttClient.subscribe(MQTT_CONFIG.topics.sensors);
                }
            }
        }
    }, 2000);  // 每2秒检查一次
}

// ================= 后端保活（防止Render实例休眠）====================
// 【新增】Render免费实例15分钟无流量会休眠，导致TTS服务中断。
// 前端定期ping后端/health保持活跃。
function startBackendKeepAlive() {
    setInterval(() => {
        fetch(`${API_BASE}/health`, { method: 'GET', cache: 'no-store' })
            .then(r => r.json())
            .then(d => {
                if (d && d.mqtt_connected !== undefined) {
                    // 后端MQTT状态记录（可在控制台观察）
                    if (!d.mqtt_connected) {
                        console.warn('[后端] MQTT未连接，TTS服务不可用');
                    } else {
                        console.log('[后端] 保活成功，MQTT已连接');
                    }
                }
            })
            .catch(e => console.warn('[后端] 保活请求失败（可能实例正在唤醒）:', e.message));
    }, 5 * 60 * 1000);  // 每5分钟ping一次
}

// ================= MQTT 连接 =================
function connectMQTT() {
    // 检查 MQTT 库是否加载
    if (typeof mqtt === 'undefined') {
        console.error('[MQTT] mqtt.min.js 未加载');
        showToast('MQTT库加载失败，请刷新页面');
        return;
    }
    console.log('[MQTT] 库版本:', mqtt.VERSION || 'unknown');

    const url = `wss://${MQTT_CONFIG.host}:${MQTT_CONFIG.port}${MQTT_CONFIG.path}`;

    try {
        mqttClient = mqtt.connect(url, {
            clientId: MQTT_CONFIG.clientId,
            username: MQTT_CONFIG.username,
            password: MQTT_CONFIG.password,
            clean: true,
            reconnectPeriod: 3000,  // 【修复】缩短重连间隔到3秒
            connectTimeout: 10000,
            rejectUnauthorized: false,  // 允许自签名证书
            protocolVersion: 4,  // MQTT 3.1.1
            keepalive: 30  // 【修复】缩短心跳间隔到30秒
        });
        console.log('[MQTT] 正在连接:', url);
    } catch (e) {
        console.error('[MQTT] 连接失败:', e);
        showToast('MQTT连接失败，请刷新重试');
        return;
    }

    mqttClient.on('connect', () => {
        AppState.mqttConnected = true;
        lastDataTime = Date.now();  // 【新增】重置心跳时间
        showToast('已接入 MQTT 数据流');
        console.log('[MQTT] 连接成功，ClientId:', MQTT_CONFIG.clientId);

        // 订阅主题
        Object.values(MQTT_CONFIG.topics).forEach(topic => {
            mqttClient.subscribe(topic, (err) => {
                if (err) {
                    console.error('[MQTT] 订阅失败:', topic, err);
                } else {
                    console.log('[MQTT] 订阅成功:', topic);
                }
            });
        });
    });

    mqttClient.on('message', async (topic, payload) => {
        lastDataTime = Date.now();  // 【新增】更新最后数据时间
        await handleMqttMessage(topic, payload);
    });

    mqttClient.on('error', (err) => {
        console.error('[MQTT] 错误:', err);
        AppState.mqttConnected = false;
    });

    mqttClient.on('offline', () => {
        console.warn('[MQTT] 连接离线');
        AppState.mqttConnected = false;
        updateModuleStatus({ main: false, vision: false, radar: false, gps: false, voice: false });
    });

    mqttClient.on('reconnect', () => {
        console.log('[MQTT] 正在重连...');
        showToast('MQTT 重连中...');
    });

    mqttClient.on('close', () => {
        console.log('[MQTT] 连接关闭');
        AppState.mqttConnected = false;
    });

    mqttClient.on('disconnect', () => {
        console.log('[MQTT] 断开连接');
        AppState.mqttConnected = false;
    });
}

// ================= MQTT 消息处理 =================
async function handleMqttMessage(topic, payload) {
    try {
        // --- 传感器数据 ---
        if (topic === MQTT_CONFIG.topics.sensors) {
            let msg;
            try {
                msg = JSON.parse(payload.toString());
            } catch (parseErr) {
                console.error('[MQTT] JSON解析失败:', parseErr.message, payload.toString().substring(0, 100));
                return;
            }

            // 调试输出（每5秒显示一次原始数据）
            const now = Date.now();
            if (!window.lastDebugTime || now - window.lastDebugTime > 5000) {
                window.lastDebugTime = now;
                console.log('[MQTT] 收到传感器数据:', JSON.stringify(msg, null, 2));
            }

            // 标记收到真实数据
            if (!AppState.deviceStarted) {
                AppState.deviceStarted = true;
                addEventLog('系统', '设备已连接，开始接收数据');
            }

            // 设备状态 - 根据真实传感器数据推断
            const hasVisionData = !!(msg.k230_class && msg.k230_class !== 'none' && msg.k230_class !== 'null');
            const hasRadarData = !!(msg.radar && (msg.radar.f !== undefined || msg.radar.front !== undefined));
            const hasGpsData = !!(msg.gps && (msg.gps.lat > 1.0 || msg.gps.lng > 1.0));
            const hasGpsSats = !!(msg.gps && (msg.gps.satellites > 0 || msg.gps.sats > 0));

            const deviceStatus = {
                main: true,
                vision: hasVisionData,
                radar: hasRadarData,
                gps: hasGpsData || hasGpsSats,
                voice: true
            };
            updateModuleStatus(deviceStatus);

            // 雷达数据（三向）- 前方/左方/右方
            if (msg.radar) {
                AppState.radarDataReceived = true;
                // 使用null作为未收到数据的标记，不再默认400
                const front = msg.radar.f !== undefined ? Number(msg.radar.f) :
                             (msg.radar.front !== undefined ? Number(msg.radar.front) : null);
                const left = msg.radar.l !== undefined ? Number(msg.radar.l) :
                            (msg.radar.left !== undefined ? Number(msg.radar.left) : null);
                const right = msg.radar.r !== undefined ? Number(msg.radar.r) :
                             (msg.radar.right !== undefined ? Number(msg.radar.right) : null);

                updateRadarCircles(front, left, right);

                // 只使用有效的雷达数据进行障碍物检测
                const validDistances = { front, left, right };
                if (front !== null || left !== null || right !== null) {
                    handleObstacleDetection(validDistances);
                }
            }

            // GPS - 兼容新旧字段名
            if (msg.gps && msg.gps.lat > 1.0 && msg.gps.lng > 1.0) {
                const speed = msg.gps.speed !== undefined ? msg.gps.speed : (msg.gps.s || 0);
                updateGPS(msg.gps.lng, msg.gps.lat, speed);
            }
            // 兼容 satellites 和 sats 两种字段名
            const satelliteCount = msg.gps.satellites !== undefined ? msg.gps.satellites : msg.gps.sats;
            if (satelliteCount !== undefined) {
                updateSatellites(satelliteCount);
            }

            // ====== K230 视觉检测数据 ======
            if (msg.k230_class && msg.k230_class !== 'none' && msg.k230_class !== 'null') {
                const cls = msg.k230_class;
                const label = msg.k230_label || cls;
                const meta = DETECTION_CLASSES[cls] || { label: label, color: '#ff4757' };
                // 检测框使用固定位置（因为串口只传类别没有bbox）
                AppState.videoDetections = [{
                    class: cls,
                    label: meta.label,
                    color: meta.color,
                    confidence: 0.85,
                    x: 80, y: 80, w: 160, h: 160
                }];
            } else {
                AppState.videoDetections = [];
            }

            // ====== 今日出行统计数据（来自ESP32）======
            if (msg.stats) {
                AppState.reportData.totalMileage = msg.stats.total_mileage || AppState.reportData.totalMileage;
                AppState.reportData.navCount = msg.stats.nav_count || AppState.reportData.navCount;
                // 【修复】障碍物提醒数取前后端较大值：ESP32重启后从RTC恢复累计值，前端也有实时检测计数，
                // 直接用ESP32值覆盖会导致前端计数被拉低/不增加
                AppState.reportData.obstacleCount = Math.max(AppState.reportData.obstacleCount, msg.stats.obstacle_count || 0);
                AppState.reportData.detourCount = msg.stats.detour_count || AppState.reportData.detourCount;

                // 更新UI显示
                document.getElementById('totalMileage').textContent = Math.round(AppState.reportData.totalMileage);
                document.getElementById('navCount').textContent = AppState.reportData.navCount;
                document.getElementById('obstacleCount').textContent = AppState.reportData.obstacleCount;
                document.getElementById('detourCount').textContent = AppState.reportData.detourCount;
            }

            // 导航状态
            const navData = {
                nav_destination: msg.nav_destination,
                nav_step:        msg.nav_step,
                current_step:    msg.current_step,
                nav_steps:       msg.nav_steps,
                nav_active:      msg.nav !== undefined ? msg.nav : msg.nav_active
            };
            updateRealtimeNav(navData);
            return;
        }

        // --- 语音分段接收 ---
        if (topic === MQTT_CONFIG.topics.voiceSeg) {
            AppState.voiceSegmentCount = parseInt(payload.toString());
            AppState.voiceSegments = [];
            AppState.voiceSegmentReceived = 0;
            return;
        }

        // --- 语音分段 PCM ---
        if (topic.startsWith('blindstick/voice/pcm/')) {
            const segIdx = parseInt(topic.split('/').pop());
            AppState.voiceSegments[segIdx] = new Uint8Array(payload);
            AppState.voiceSegmentReceived++;

            // 如果收齐所有段
            if (AppState.voiceSegmentReceived >= AppState.voiceSegmentCount && AppState.voiceSegmentCount > 0) {
                let totalLen = 0;
                for (const seg of AppState.voiceSegments) {
                    if (seg) totalLen += seg.length;
                }
                const fullPcm = new Uint8Array(totalLen);
                let offset = 0;
                for (const seg of AppState.voiceSegments) {
                    if (seg) {
                        fullPcm.set(seg, offset);
                        offset += seg.length;
                    }
                }
                handleVoiceNavigation(fullPcm);
                AppState.voiceSegmentCount = 0;
            }
            return;
        }

        // --- TTS 音频（来自 ESP32 通过 MQTT 代理的 TTS 结果）---
        if (topic === MQTT_CONFIG.topics.ttsAudio) {
            return;
        }

        // --- TTS 请求（ESP32通过MQTT代理请求TTS）---
        // 检测开机语音请求，记录设备启动事件
        if (topic === MQTT_CONFIG.topics.ttsReq) {
            try {
                const msg = JSON.parse(payload.toString());
                // 检测到开机语音请求，记录设备启动
                if (msg.text && msg.text.includes('系统启动成功') && !AppState.deviceStarted) {
                    AppState.deviceStarted = true;
                    addEventLog('系统', '设备启动成功');
                }
            } catch (e) {
                console.error('[TTS-MQTT] 解析失败:', e);
            }
            return;
        }

        // --- 导航路线 ---
        if (topic === MQTT_CONFIG.topics.navSteps) {
            const msg = JSON.parse(payload.toString());
            if (msg.destination && msg.steps) {
                // 路线调整统计由ESP32负责并上报，前端不再重复统计
                AppState.navJustStarted = false;
                updateNavigationSteps(msg);
                addNavHistory(msg.destination, msg.steps);
            }
            return;
        }
    } catch (e) {
        console.error('[MQTT] 报文解析异常:', e);
    }
}

// ================= UI 状态更新函数 =================
function updateModuleStatus(modules) {
    const statusMap = {
        main: 'status-main',
        vision: 'status-vision',
        radar: 'status-radar',
        gps: 'status-gps',
        voice: 'status-voice'
    };

    for (const [key, elementId] of Object.entries(statusMap)) {
        if (modules[key] !== undefined) {
            const el = document.getElementById(elementId);
            if (el) {
                el.className = modules[key] ? 'status-item online' : 'status-item offline';
            }
        }
    }
}

// ==================== 三向雷达 UI 更新 ====================
function updateRadarCircles(front, left, right) {
    const valFront = document.getElementById('valFront');
    const valLeft = document.getElementById('valLeft');
    const valRight = document.getElementById('valRight');
    const barFront = document.getElementById('barFront');
    const barLeft = document.getElementById('barLeft');
    const barRight = document.getElementById('barRight');

    // 显示 400cm 如果数据为null，否则显示实际数值
    if (valFront) valFront.textContent = front !== null ? Math.round(front) + 'cm' : '400cm';
    if (valLeft) valLeft.textContent = left !== null ? Math.round(left) + 'cm' : '400cm';
    if (valRight) valRight.textContent = right !== null ? Math.round(right) + 'cm' : '400cm';

    // 更新进度条（如果数据有效）
    if (barFront && front !== null) updateRadarBar(barFront, front);
    if (barLeft && left !== null) updateRadarBar(barLeft, left);
    if (barRight && right !== null) updateRadarBar(barRight, right);
}

function updateRadarBar(bar, distance) {
    const maxDist = 400;
    const pct = Math.min(100, Math.max(0, (distance / maxDist) * 100));
    bar.style.width = pct + '%';
    bar.className = 'dist-bar';
    if (distance < 180)      bar.classList.add('danger');
    else if (distance < 300) bar.classList.add('warn');
    else                     bar.classList.add('safe');
}

function hexToRgba(hex, alpha) {
    const r = parseInt(hex.slice(1, 3), 16);
    const g = parseInt(hex.slice(3, 5), 16);
    const b = parseInt(hex.slice(5, 7), 16);
    return `rgba(${r},${g},${b},${alpha})`;
}

function drawVideoFrame() {
    const canvas = document.getElementById('videoCanvas');
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    ctx.fillStyle = '#1a1a2e';
    ctx.fillRect(0, 0, canvas.width, canvas.height);
    if (AppState.videoDetections.length === 0) {
        ctx.fillStyle = 'rgba(255,255,255,0.25)';
        ctx.font = '14px sans-serif'; ctx.textAlign = 'center';
        ctx.fillText('等待检测数据...', canvas.width / 2, canvas.height / 2 + 5);
        updateDetectionStats({}); return;
    }
    const counts = {};
    CATEGORY_GROUPS.forEach(g => counts[g.key] = 0);
    AppState.videoDetections.forEach(det => {
        const imgW = AppState.imgSize[0] || 320, imgH = AppState.imgSize[1] || 320;
        const scaleX = canvas.width / imgW, scaleY = canvas.height / imgH;
        const rx = det.x * scaleX, ry = det.y * scaleY;
        const rw = det.w * scaleX, rh = det.h * scaleY;
        const color = det.color || '#ef4444';
        const label = det.label || det.class || 'unknown';
        ctx.strokeStyle = color; ctx.lineWidth = 2.5;
        ctx.strokeRect(rx, ry, rw, rh);
        ctx.fillStyle = hexToRgba(color, 0.15); ctx.fillRect(rx, ry, rw, rh);
        const txt = `${label} ${Math.round((det.confidence || 0) * 100)}%`;
        ctx.font = 'bold 12px sans-serif';
        const tw = ctx.measureText(txt).width;
        ctx.fillStyle = hexToRgba(color, 0.85); ctx.fillRect(rx, ry - 22, tw + 10, 22);
        ctx.fillStyle = '#ffffff'; ctx.fillText(txt, rx + 5, ry - 7);
        ctx.strokeStyle = color; ctx.lineWidth = 2;
        ctx.beginPath(); ctx.moveTo(rx+6, ry); ctx.lineTo(rx, ry); ctx.lineTo(rx, ry+6); ctx.stroke();
        ctx.beginPath(); ctx.moveTo(rx+rw-6, ry+rh); ctx.lineTo(rx+rw, ry+rh); ctx.lineTo(rx+rw, ry+rh-6); ctx.stroke();
        const cls = det.class || 'unknown';
        if (counts[cls] !== undefined) counts[cls]++;
    });
    updateDetectionStats(counts);
}

function initDetectionStats() {
    const container = document.getElementById('detectionStats');
    if (!container) return;
    let html = '';
    CATEGORY_GROUPS.forEach(g => {
        const cls = DETECTION_CLASSES[g.key] || { color: '#888888' };
        html += `<div class="stat" data-class="${g.key}" style="border-left:3px solid ${cls.color};padding-left:6px;">${g.label} <span class="stat-count">0</span></div>`;
    });
    container.innerHTML = html;
}

function updateDetectionStats(counts) {
    CATEGORY_GROUPS.forEach(g => {
        const el = document.querySelector(`.stat[data-class="${g.key}"] .stat-count`);
        if (el) el.textContent = counts[g.key] || 0;
    });

    // 更新 FPS/检测状态显示
    const fpsEl = document.getElementById('fpsDisplay');
    if (fpsEl) {
        const hasDetections = Object.values(counts).some(c => c > 0);
        fpsEl.textContent = hasDetections ? '检测中' : '等待中';
        fpsEl.className = 'tag tag-fps ' + (hasDetections ? 'active' : '');
    }
}

// ================= 地图 =================
let map, pathPolyline, marker;
function initMap() {
    const mapEl = document.getElementById('map');
    if (!mapEl) return;
    // 初始视图使用中国中心，等待GPS数据
    const defaultCenter = [35.0, 105.0];
    map = L.map('map', { zoomControl: false }).setView(defaultCenter, 4);
    L.tileLayer('https://webrd0{s}.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1&style=8&x={x}&y={y}&z={z}', {
        subdomains: ['1', '2', '3', '4'], attribution: '© 高德地图'
    }).addTo(map);
    pathPolyline = L.polyline([], { color: '#2563eb', weight: 4 }).addTo(map);
    marker = L.circleMarker(defaultCenter, { color: '#2563eb', radius: 6, fillColor: '#fff', fillOpacity: 1, weight: 3 }).addTo(map);
}

function updateGPS(lng, lat, speed) {
    const pos = [lat, lng];

    // 更新地图中心（仅在首次接收到有效GPS时）
    if (map && !AppState.gpsInitialized) {
        map.setView(pos, 15);
        AppState.gpsInitialized = true;
    }

    if (marker) marker.setLatLng(pos);
    if (pathPolyline) pathPolyline.addLatLng(pos);
    if (map) map.panTo(pos);

    const lngEl = document.getElementById('lng');
    const latEl = document.getElementById('lat');
    const speedEl = document.getElementById('speed');
    if (lngEl) lngEl.textContent = lng.toFixed(6);
    if (latEl) latEl.textContent = lat.toFixed(6);
    if (speedEl) speedEl.textContent = (speed || 0).toFixed(1);

    // 更新状态为GPS已定位
    if (!AppState.gpsHasFix) {
        AppState.gpsHasFix = true;
    }

    // 计算里程
    if (AppState.lastGpsPos) {
        const dist = calcDistance(AppState.lastGpsPos.lat, AppState.lastGpsPos.lng, lat, lng);
        if (dist > 1 && dist < 100) {
            AppState.reportData.totalMileage += dist;
            document.getElementById('totalMileage').textContent = Math.round(AppState.reportData.totalMileage);
        }
    }
    AppState.lastGpsPos = { lat, lng };
}

// ================= 导航 =================
function updateRealtimeNav(data) {
    const destEl = document.getElementById('navDestination');
    const stepTextEl = document.getElementById('navStepText');
    const currentNumEl = document.getElementById('currentStepNum');
    const totalNumEl = document.getElementById('totalStepNum');
    const progressBar = document.getElementById('navProgressBar');
    const statusBadge = document.getElementById('navStatusBadge');

    const navDestination = data.nav_destination;
    const navStep = data.nav_step;
    const currentStep = data.current_step;
    const navSteps = data.nav_steps;
    const navActive = data.nav_active;

    if (navActive === false) {
        if (destEl) destEl.textContent = '--';
        if (stepTextEl) stepTextEl.textContent = '等待导航开始...';
        if (currentNumEl) currentNumEl.textContent = '0';
        if (totalNumEl) totalNumEl.textContent = '0';
        if (progressBar) progressBar.style.width = '0%';
        if (statusBadge) { statusBadge.textContent = '等待中'; statusBadge.className = 'badge'; }
        document.getElementById('stopNavBtn').style.display = 'none';
        return;
    }

    if (destEl && navDestination) destEl.textContent = navDestination;
    if (stepTextEl && navStep) stepTextEl.textContent = navStep;
    if (currentNumEl && currentStep !== undefined) currentNumEl.textContent = currentStep + 1;
    if (totalNumEl && navSteps) totalNumEl.textContent = navSteps.length;
    if (progressBar && currentStep !== undefined && navSteps && navSteps.length > 0)
        progressBar.style.width = ((currentStep + 1) / navSteps.length) * 100 + '%';
    if (statusBadge) {
        if (navActive) { statusBadge.textContent = '导航中'; statusBadge.className = 'badge active'; }
        else          { statusBadge.textContent = '等待中'; statusBadge.className = 'badge'; }
    }
    const stopNavBtn = document.getElementById('stopNavBtn');
    if (stopNavBtn) stopNavBtn.style.display = navActive ? 'block' : 'none';
}

function updateNavigationSteps(msg) {
    const stepsList = document.getElementById('navStepsList');
    if (!stepsList) return;
    let steps = msg.steps || msg.nav_steps || (msg.data && msg.data.nav_steps) || [];
    let currentStep = msg.current_step || msg.currentStep || 0;
    const navActive = msg.nav_active !== undefined ? msg.nav_active : (msg.data && msg.data.nav_active);
    if (navActive === false || !steps || steps.length === 0) { stepsList.innerHTML = ''; return; }
    let html = '';
    steps.forEach((step, idx) => {
        const isCurrent = idx === currentStep;
        const isCompleted = idx < currentStep;
        const cls = isCurrent ? 'current' : (isCompleted ? 'completed' : '');
        html += `<div class="nav-step-item ${cls}"><div class="nav-step-num">${idx+1}</div><div class="nav-step-desc">${step}</div></div>`;
    });
    stepsList.innerHTML = html;
}

function updateSatellites(count) {
    const satEl = document.getElementById('satelliteCount');
    const gpsEl = document.getElementById('gpsStatus');
    if (satEl) satEl.textContent = `卫星 ${count}`;
    if (gpsEl) {
        if (count >= 4)      { gpsEl.textContent = 'GPS信号良好'; gpsEl.className = 'tag'; }
        else if (count > 0)  { gpsEl.textContent = 'GPS信号弱';   gpsEl.className = 'tag warn'; }
        else                 { gpsEl.textContent = 'GPS搜星中';   gpsEl.className = 'tag warn'; }
    }
    updateModuleStatus({ gps: count > 0 });
}

// ================= 导航记录 =================
function addNavHistory(destination, steps) {
    const el = document.getElementById('navHistoryList');
    if (!el) return;

    // 检测是否是路线调整（如果已经在导航中，又发起新导航）
    const isDetour = AppState.reportData.navCount > 0 && !AppState.navJustStarted;

    const time = new Date().toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit' });
    const item = document.createElement('div');
    item.className = 'nav-history-item';
    item.innerHTML = `<div class="nav-history-header"><span class="nav-time">${time}</span><span class="nav-dest">${destination}</span><span class="nav-steps-count">${steps ? steps.length : 0} 步</span></div><div class="nav-history-steps">${steps ? steps[0] : '无详情'}</div>`;
    const empty = el.querySelector('.nav-empty');
    if (empty) empty.remove();
    el.insertBefore(item, el.firstChild);

    // 增加导航次数计数
    AppState.reportData.navCount++;
    document.getElementById('navCount').textContent = AppState.reportData.navCount;

    // 如果是路线调整，通知ESP32
    if (isDetour && mqttClient && AppState.mqttConnected) {
        const detourMsg = JSON.stringify({
            type: 'detour',
            ts: Date.now()
        });
        mqttClient.publish('blindstick/stats/detour', detourMsg);
        addEventLog('导航', '路线已调整');
    }

    // 记录导航开始时间（用于路线调整检测）
    AppState.navStartTime = Date.now();
    // 标记导航刚开始（防止第一次路线被算作调整）
    AppState.navJustStarted = true;
}

// ================= 初始化 =================
function initClock() {
    // 时钟功能已移除，保留空函数避免报错
}

function initModal() {
    // 弹窗初始化
    const modal = document.getElementById('settingsModal');
    if (modal) {
        modal.addEventListener('click', (e) => {
            if (e.target.id === 'settingsModal') closeSettings();
        });
    }
}

function showToast(msg) {
    // 显示提示消息
    const t = document.createElement('div');
    t.style.cssText = 'position:fixed;bottom:24px;left:50%;transform:translateX(-50%);background:rgba(0,0,0,0.8);color:#fff;padding:8px 16px;border-radius:8px;font-size:13px;z-index:9999;transition:opacity 0.3s;';
    t.textContent = msg;
    document.body.appendChild(t);
    setTimeout(() => t.remove(), 3000);
}

function init() {
    initClock();
    initMap();
    initDetectionStats();
    connectMQTT();
    startHeartbeatCheck();  // 【新增】启动心跳检测
    startBackendKeepAlive();  // 【新增】启动后端保活
    loadHomeCitySettings();
    initModal();

    function loop() {
        if (!AppState.isRunning) return;
        drawVideoFrame();
        requestAnimationFrame(loop);
    }
    requestAnimationFrame(loop);
}

document.addEventListener('DOMContentLoaded', init);

// ================= 新增：改进的目的地提取功能 =================

function extractDestinationAdvanced(text) {
    if (!text || text.length < 2) return null;

    // 步骤1：查找触发词
    let triggerIndex = -1;
    let matchedTrigger = '';

    for (const trigger of NAVIGATION_TRIGGERS) {
        const idx = text.indexOf(trigger);
        if (idx !== -1 && (triggerIndex === -1 || idx < triggerIndex)) {
            triggerIndex = idx;
            matchedTrigger = trigger;
        }
    }

    if (triggerIndex === -1) {
        return null;
    }

    // 步骤2：提取触发词后的内容
    let destination = text.substring(triggerIndex + matchedTrigger.length).trim();

    // 步骤3：去除标点符号
    destination = destination.replace(/[，。？！.,?!；：""''（）()【】\[\]{}]/g, ' ');

    // 步骤4：按空格分割，取第一个非空部分
    const parts = destination.split(/\s+/).filter(p => p.length > 0);
    if (parts.length === 0) {
        return null;
    }

    destination = parts[0];

    // 步骤5：过滤非目的地词汇
    let filtered = destination;
    for (const word of DESTINATION_FILTER_WORDS) {
        if (filtered === word) {
            filtered = '';
            break;
        }
        if (filtered.startsWith(word)) {
            filtered = filtered.substring(word.length);
        }
        if (filtered.endsWith(word)) {
            filtered = filtered.substring(0, filtered.length - word.length);
        }
    }

    filtered = filtered.trim();

    // 步骤6：验证目的地有效性
    if (filtered.length < 2) {
        return null;
    }

    // 不能全是数字或标点
    if (/^[\d\s\p{P}]+$/u.test(filtered)) {
        return null;
    }

    return filtered;
}

// ================= 新增：搜索最近目的地功能 =================

async function searchNearestDestination(keyword, currentLat, currentLng) {
    try {
        const searchUrl = `https://api.map.baidu.com/place/v2/search?query=${encodeURIComponent(keyword)}&region=${encodeURIComponent(API_CONFIG.homeCity)}&output=json&ak=${API_CONFIG.baiduMapAk}&page_size=10`;

        const searchRes = await fetch(searchUrl);
        const searchData = await searchRes.json();

        if (searchData.status !== 0 || !searchData.results || searchData.results.length === 0) {
            return null;
        }

        // 如果没有当前位置，返回第一个结果
        if (!currentLat || !currentLng) {
            return searchData.results[0];
        }

        // 计算每个结果与当前位置的距离
        let nearest = null;
        let minDistance = Infinity;

        for (const place of searchData.results) {
            if (!place.location || !place.location.lat || !place.location.lng) {
                continue;
            }

            const distance = calcDistance(
                currentLat, currentLng,
                place.location.lat, place.location.lng
            );

            place._distance = distance;

            if (distance < minDistance) {
                minDistance = distance;
                nearest = place;
            }
        }

        return nearest;

    } catch (e) {
        console.error('[最近目的地] 搜索失败:', e);
        return null;
    }
}

async function planRouteToNearest(destination, currentLat, currentLng) {
    const nearestPlace = await searchNearestDestination(destination, currentLat, currentLng);

    if (!nearestPlace) {
        return null;
    }

    const distance = nearestPlace._distance || 0;

    if (distance > MAX_NAVIGATION_DISTANCE) {
        return {
            tooFar: true,
            distance: distance,
            destination: nearestPlace.name,
            message: `目的地${nearestPlace.name}距离您${Math.round(distance / 1000)}公里，距离太远`
        };
    }

    try {
        const origin = currentLat && currentLng ? `${currentLat},${currentLng}` : '30.229320,115.063977';
        const destLat = nearestPlace.location.lat;
        const destLng = nearestPlace.location.lng;

        const routeUrl = `https://api.map.baidu.com/directionlite/v1/walking?origin=${origin}&destination=${destLat},${destLng}&ak=${API_CONFIG.baiduMapAk}`;

        const routeRes = await fetch(routeUrl);
        const routeData = await routeRes.json();

        if (routeData.status !== 0 || !routeData.result || !routeData.result.routes || routeData.result.routes.length === 0) {
            return null;
        }

        const route = routeData.result.routes[0];
        const steps = route.steps.map(s => s.instruction.replace(/<[^>]+>/g, ''));

        return {
            destination: nearestPlace.name,
            destinationAddress: nearestPlace.address || '',
            steps: steps,
            distance: route.distance,
            duration: route.duration,
            destLat: destLat,
            destLng: destLng,
            straightDistance: distance
        };

    } catch (e) {
        console.error('[路线规划] 请求失败:', e);
        return null;
    }
}

// ================= 新增：事件记录功能 =================

function addEventLog(category, message) {
    const eventList = document.getElementById('eventList');
    if (!eventList) {
        return;
    }

    const classMap = {
        '系统': 'info',
        '导航': 'success',
        '障碍物': 'danger',
        '语音': 'info',
        '雷达': 'danger'
    };
    const itemClass = classMap[category] || 'info';
    const time = new Date().toLocaleTimeString('zh-CN', { hour: '2-digit', minute: '2-digit', second: '2-digit' });

    const item = document.createElement('div');
    item.className = `event-item ${itemClass}`;
    // 【修复】障碍物事件不显示"障碍物"标题，直接显示检测描述（如"右方检测到障碍物，距离 42cm"）
    const titleHtml = (category === '障碍物') ? '' : `<div class="event-title">${category}</div>`;
    item.innerHTML = `
        <div class="event-time">${time}</div>
        <div class="event-content">
            ${titleHtml}
            <div class="event-desc">${message}</div>
        </div>
    `;

    eventList.insertBefore(item, eventList.firstChild);

    while (eventList.children.length > 50) {
        eventList.removeChild(eventList.lastChild);
    }
}

function clearEventLog() {
    const eventList = document.getElementById('eventList');
    if (eventList) {
        eventList.innerHTML = '';
    }
}

function clearChatHistory() {
    const chatContainer = document.getElementById('chatContainer');
    if (chatContainer) {
        chatContainer.innerHTML = '<div class="chat-welcome">语音对话由ESP32硬件处理</div>';
    }
}

function clearNavHistory() {
    const navHistoryList = document.getElementById('navHistoryList');
    if (navHistoryList) {
        navHistoryList.innerHTML = '<div class="nav-empty">暂无导航记录</div>';
    }
}

// ================= 新增：开机播报（已由ESP32硬件端处理）====================
// 注意：开机语音由ESP32硬件端处理，前端只检测TTS请求中的开机消息来记录事件

// ================= 障碍物检测（仅用于统计和显示，不播报）====================

function handleObstacleDetection(radarData) {
    const { front, left, right } = radarData;
    const OBSTACLE_THRESHOLD = 100;

    // 找出最近的障碍物（只使用有效的数据）
    const distances = [];
    if (front !== null) distances.push({ dist: front, dir: '前方' });
    if (left !== null) distances.push({ dist: left, dir: '左方' });
    if (right !== null) distances.push({ dist: right, dir: '右方' });

    if (distances.length === 0) return;

    let minDist = Infinity;
    let minDir = '';

    for (const item of distances) {
        if (item.dist < minDist) {
            minDist = item.dist;
            minDir = item.dir;
        }
    }

    // 只在状态变化时更新计数（用于统计）
    if (minDist < OBSTACLE_THRESHOLD && !AppState.lastObstacleState) {
        AppState.reportData.obstacleCount++;
        document.getElementById('obstacleCount').textContent = AppState.reportData.obstacleCount;
        AppState.lastObstacleState = true;
        addEventLog('障碍物', `${minDir}检测到障碍物，距离 ${Math.round(minDist)}cm`);
    } else if (minDist >= OBSTACLE_THRESHOLD + 20) {
        AppState.lastObstacleState = false;
    }
}

// ================= 语音导航处理（改进版 - ESP32播放）====================

async function handleVoiceNavigationAdvanced(text) {
    addEventLog('语音', `识别: ${text}`);

    const destination = extractDestinationAdvanced(text);

    if (!destination) {
        addEventLog('语音', '未提取到有效目的地');
        await baiduTTS('请说出具体地点，例如带我去天安门');
        return;
    }

    addEventLog('导航', `目的地: ${destination}`);

    const currentPos = AppState.lastGpsPos;

    const route = await planRouteToNearest(destination, currentPos?.lat, currentPos?.lng);

    if (!route) {
        addEventLog('导航', '路线规划失败');
        await baiduTTS('抱歉，没有找到该地点的路线');
        return;
    }

    if (route.tooFar) {
        addEventLog('导航', route.message);
        await baiduTTS(route.message + '，请重新选择较近的地点');
        return;
    }

    const navMsg = {
        status: 'ok',
        destination: route.destination,
        steps: route.steps,
        distance: route.distance,
        duration: route.duration,
        ts: Date.now()
    };

    if (mqttClient && AppState.mqttConnected) {
        mqttClient.publish(MQTT_CONFIG.topics.navSteps, JSON.stringify(navMsg));
        addEventLog('导航', `开始导航到 ${route.destination}，共${route.steps.length}步，${Math.round(route.distance)}米`);
    }

    const firstStep = route.steps[0] || '开始导航';
    const ttsText = `开始导航到${route.destination}，全程${Math.round(route.distance)}米，预计${Math.round(route.duration / 60)}分钟，${firstStep}`;
    await baiduTTS(ttsText);

    updateNavigationSteps(navMsg);
    addNavHistory(route.destination, route.steps);
}

// ================= 停止导航功能 =================

function stopNavigation() {
    if (!mqttClient || !AppState.mqttConnected) {
        showToast('MQTT未连接，无法发送停止指令');
        return;
    }

    const stopMsg = {
        nav_active: false,
        status: 'stop',
        steps: [],
        current_step: 0,
        destination: null,
        timestamp: Date.now()
    };

    mqttClient.publish(MQTT_CONFIG.topics.navSteps, JSON.stringify(stopMsg), (err) => {
        if (err) {
            showToast('停止导航失败，请重试');
        } else {
            updateNavigationSteps({
                nav_active: false,
                steps: [],
                current_step: 0,
                destination: null
            });

            showToast('导航已停止');
            addEventLog('导航', '用户手动停止导航');
        }
    });
}

// ================= 新增：初始化时播放开机播报 =================

// 导出函数供外部使用
if (typeof window !== 'undefined') {
    window.extractDestinationAdvanced = extractDestinationAdvanced;
    window.planRouteToNearest = planRouteToNearest;
    window.handleVoiceNavigationAdvanced = handleVoiceNavigationAdvanced;
    window.addEventLog = addEventLog;
    window.clearEventLog = clearEventLog;
    window.clearChatHistory = clearChatHistory;
    window.clearNavHistory = clearNavHistory;
    window.stopNavigation = stopNavigation;
}

// ================= 新增：常住地设置功能 =================

function openSettings() {
    const modal = document.getElementById('settingsModal');
    const currentCityDiv = document.getElementById('currentHomeCity');
    const homeCityInput = document.getElementById('homeCityInput');

    const savedCity = localStorage.getItem('homeCity') || API_CONFIG.homeCity;
    if (currentCityDiv) {
        currentCityDiv.textContent = savedCity;
    }
    if (homeCityInput) {
        homeCityInput.value = '';
        homeCityInput.placeholder = `例如：${savedCity}`;
    }

    modal.style.display = 'flex';
}

function closeSettings() {
    const modal = document.getElementById('settingsModal');
    modal.style.display = 'none';
}

function saveSettings() {
    const homeCityInput = document.getElementById('homeCityInput');
    const newCity = homeCityInput.value.trim();

    if (!newCity) {
        showToast('请输入常住地城市名称');
        return;
    }

    localStorage.setItem('homeCity', newCity);
    API_CONFIG.homeCity = newCity;
    AppState.config.homeCity = newCity;

    if (mqttClient && AppState.mqttConnected) {
        const msg = JSON.stringify({
            type: 'home_city_update',
            city: newCity,
            ts: Date.now()
        });
        mqttClient.publish('blindstick/config/home_city', msg);
    }

    const currentCityDiv = document.getElementById('currentHomeCity');
    if (currentCityDiv) {
        currentCityDiv.textContent = newCity;
    }

    showToast(`常住地已设置为：${newCity}`);
    closeSettings();
}

function loadHomeCitySettings() {
    const savedCity = localStorage.getItem('homeCity');
    if (savedCity) {
        API_CONFIG.homeCity = savedCity;
        AppState.config.homeCity = savedCity;
    }
}

// ================= 手动重连 MQTT =================
function reconnectMQTT() {
    if (mqttClient) {
        console.log('[MQTT] 手动断开并重连...');
        mqttClient.end(true, () => {
            console.log('[MQTT] 已断开，重新连接...');
            connectMQTT();
        });
    } else {
        connectMQTT();
    }
}

// 导出到全局供HTML调用
if (typeof window !== 'undefined') {
    window.reconnectMQTT = reconnectMQTT;
}
