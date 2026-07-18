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
    gpsCenter: [30.23193, 115.05791],
    reportData: {
        totalMileage: 0, navCount: 0, obstacleCount: 0, detourCount: 0,
    },
    mqttConnected: false,
    imgSize: [320, 320],
    navHistory: [],
    config: { homeCity: '黄石市' },
    // 统计计数辅助
    lastObstacleState: false,  // 上一次是否有障碍物（防止重复计数）
    lastGpsPos: null,          // 上一次GPS位置（用于计算里程）
    navStartTime: null,        // 导航开始时间
    navJustStarted: false,     // 导航是否刚开始（用于区分路线调整）
    // 语音分段接收
    voiceSegments: [],
    voiceSegmentCount: 0,
    voiceSegmentReceived: 0
};

// ================= 计算两点间距离（米）====================
function calcDistance(lat1, lng1, lat2, lng2) {
    const R = 6371000; // 地球半径（米）
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
const MAX_NAVIGATION_DISTANCE = 10000; // 10公里

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
    console.log('[百度TTS] 请求合成:', text);

    try {
        const response = await fetch(`${API_BASE}/api/tts`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ text })
        });

        if (response.ok) {
            const audioData = await response.arrayBuffer();
            console.log('[百度TTS] 合成成功:', audioData.byteLength, '字节');
            // 返回音频数据，供MQTT发送给ESP32
            return new Uint8Array(audioData);
        } else {
            const error = await response.json();
            console.error('[百度TTS] 错误:', error);
            return null;
        }
    } catch (e) {
        console.error('[百度TTS] 请求异常:', e.message);
        return null;
    }
}

// --- 百度 TTS（ESP32直接调用，网页端只负责转发音频数据给ESP32）---
// 注意：ESP32优先直接调用百度API，当ESP32无法连接百度时，网页端代为合成并通过MQTT发送音频
async function baiduTTS(text) {
    console.log('[百度TTS] 合成文本:', text);
    // 通过MQTT发送文本给ESP32，让ESP32自己合成并播放
    if (mqttClient && AppState.mqttConnected) {
        const msg = JSON.stringify({
            type: 'tts_request',
            text: text
        });
        mqttClient.publish(MQTT_CONFIG.topics.ttsReq, msg);
        console.log('[百度TTS] 已发送文本给ESP32:', text);
    }
    return null;
}

// ==================== 流式TTS（新）====================
// 优先级定义
const TTS_PRIORITY = {
    LOW: 0,      // 导航
    NORMAL: 1,   // 对话
    HIGH: 2      // 雷达告警
};

// 当前TTS会话状态
let currentTTSSession = {
    sessionId: null,
    priority: 0,
    isPlaying: false
};

/**
 * 发送TTS请求到代理服务器进行流式合成
 * @param {string} text - 要合成的文本
 * @param {number} priority - 优先级 0=低(导航) 1=中(对话) 2=高(雷达告警)
 * @returns {Promise<boolean>}
 */
async function streamTTS(text, priority = TTS_PRIORITY.NORMAL) {
    console.log(`[流式TTS] 请求: "${text}" 优先级=${priority}`);

    if (!mqttClient || !AppState.mqttConnected) {
        console.error('[流式TTS] MQTT未连接');
        return false;
    }

    try {
        // 调用代理服务器的推送接口
        const response = await fetch(`${API_BASE}/api/tts/push`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ text, priority })
        });

        const result = await response.json();
        if (result.status === 'ok') {
            console.log('[流式TTS] 已推送到ESP32:', text.substring(0, 30));
            return true;
        } else {
            console.error('[流式TTS] 推送失败:', result.message);
            return false;
        }
    } catch (e) {
        console.error('[流式TTS] 请求异常:', e.message);
        // 降级到普通TTS
        return baiduTTS(text);
    }
}

/**
 * 立即打断当前TTS播放（用于雷达告警等紧急场景）
 * @param {number} newPriority - 新播放的优先级
 */
async function interruptTTS(newPriority = TTS_PRIORITY.HIGH) {
    console.log(`[流式TTS] 打断请求，新优先级=${newPriority}`);

    try {
        await fetch(`${API_BASE}/api/tts/interrupt`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ priority: newPriority })
        });
    } catch (e) {
        console.error('[流式TTS] 打断请求失败:', e.message);
    }
}

/**
 * 发送雷达告警语音（高优先级，立即打断）
 * @param {number} distance - 距离（厘米）
 * @param {string} direction - 方向
 */
async function announceObstacle(distance, direction) {
    let text;
    if (distance < 150) {
        text = `注意！前方${Math.round(distance)}厘米有障碍物，请立即避让！`;
    } else {
        text = `前方${Math.round(distance)}厘米有${direction}障碍物`;
    }

    console.log('[雷达告警]', text);
    await streamTTS(text, TTS_PRIORITY.HIGH);
}

/**
 * 发送导航语音（普通优先级）
 * @param {string} text - 导航文本
 */
async function announceNavigation(text) {
    console.log('[导航语音]', text);
    await streamTTS(text, TTS_PRIORITY.LOW);
}

// --- 处理语音导航（简化版 - 调用改进版）---
async function handleVoiceNavigation(pcmBytes) {
    console.log('[语音] 收到PCM数据', pcmBytes.length, '字节');

    // 语音识别
    const text = await baiduASR(pcmBytes);
    if (!text) {
        await streamTTS('语音识别失败，请重新说出目的地', TTS_PRIORITY.NORMAL);
        return;
    }

    // 调用改进版处理函数
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

// ================= MQTT 连接 =================
function connectMQTT() {
    const url = `wss://${MQTT_CONFIG.host}:${MQTT_CONFIG.port}${MQTT_CONFIG.path}`;
    console.log('[MQTT] 正在连接:', url);

    try {
        mqttClient = mqtt.connect(url, {
            clientId: MQTT_CONFIG.clientId,
            username: MQTT_CONFIG.username,
            password: MQTT_CONFIG.password,
            clean: true,
            reconnectPeriod: 5000,
            connectTimeout: 10000
        });
    } catch (e) {
        console.error('[MQTT] 连接失败:', e);
        showToast('MQTT连接失败，请刷新重试');
        return;
    }

    mqttClient.on('connect', () => {
        AppState.mqttConnected = true;
        console.log('[MQTT] 已连接');
        showToast('已接入 MQTT 数据流');

        // 订阅主题
        Object.values(MQTT_CONFIG.topics).forEach(topic => {
            mqttClient.subscribe(topic, (err) => {
                if (err) console.error('[MQTT] 订阅失败:', topic, err);
                else console.log('[MQTT] 已订阅:', topic);
            });
        });

        // 播放开机播报
        setTimeout(() => {
            playStartupSound();
        }, 2000);

        // 订阅确认后，发送测试消息
        setTimeout(() => {
            console.log('[MQTT] 订阅完成，测试发布到:', MQTT_CONFIG.topics.ttsReq);
        }, 1000);
    });

    mqttClient.on('message', async (topic, payload) => {
        console.log('[MQTT] 收到:', topic, payload.length + ' bytes');
        await handleMqttMessage(topic, payload);
    });

    mqttClient.on('error', (err) => {
        console.error('[MQTT] 错误:', err);
        AppState.mqttConnected = false;
    });

    mqttClient.on('offline', () => {
        console.warn('[MQTT] 离线，等待重连...');
        AppState.mqttConnected = false;
        updateModuleStatus({ main: false, vision: false, radar: false, gps: false, voice: false });
    });

    mqttClient.on('reconnect', () => {
        console.log('[MQTT] 重连中...');
        showToast('MQTT 重连中...');
    });
}

// ================= MQTT 消息处理 =================
async function handleMqttMessage(topic, payload) {
    try {
        // --- 传感器数据 ---
        if (topic === MQTT_CONFIG.topics.sensors) {
            const msg = JSON.parse(payload.toString());
            console.log('[MQTT] 传感器数据:', JSON.stringify(msg));

            // 设备状态 - 根据真实传感器数据推断
            // ESP32 没有直接发送 status 对象，需要根据各传感器数据推断
            const deviceStatus = {
                main: true,  // ESP32 在线（能收到MQTT消息就是在线）
                vision: false,  // K230 暂时未接入，后面用户会提供
                radar: !!(msg.radar && (msg.radar.f !== undefined || msg.radar.front !== undefined)),
                gps: !!(msg.gps && msg.gps.sats > 0),
                voice: true  // 语音模块在线（能处理TTS请求）
            };
            updateModuleStatus(deviceStatus);

            // 雷达数据（五向）- 简化为短字段名
            if (msg.radar) {
                // 强制转换为数字，处理字符串或 undefined 情况
                const front = Number(msg.radar.f ?? msg.radar.front ?? 400);
                const frontLeft = Number(msg.radar.fl ?? msg.radar.frontLeft ?? 400);
                const frontRight = Number(msg.radar.fr ?? msg.radar.frontRight ?? 400);
                const left = Number(msg.radar.l ?? msg.radar.left ?? 400);
                const right = Number(msg.radar.r ?? msg.radar.right ?? 400);

                // 详细调试输出
                console.log('[雷达原始]', JSON.stringify(msg.radar));
                console.log('[雷达原始r值]', msg.radar.r, '类型:', typeof msg.radar.r);
                console.log('[雷达解析] F:%d FL:%d FR:%d L:%d R:%d', front, frontLeft, frontRight, left, right);

                updateRadarCircles(front, frontLeft, frontRight, left, right);

                // 使用新的障碍物检测和播报功能
                handleObstacleDetection({ front, frontLeft, frontRight, left, right });
            }

            // GPS
            if (msg.gps && msg.gps.lat > 1.0 && msg.gps.lng > 1.0) {
                updateGPS(msg.gps.lng, msg.gps.lat, msg.gps.speed || 0);
            }
            if (msg.gps && msg.gps.satellites !== undefined) {
                updateSatellites(msg.gps.satellites);
            }

            // ====== K230 视觉检测数据 ======
            // ESP32通过MQTT发送的K230数据格式: { k230_class, k230_label, k230_danger }
            if (msg.k230_class && msg.k230_class !== 'none' && msg.k230_class !== 'null') {
                const cls = msg.k230_class;
                const label = msg.k230_label || cls;
                const meta = DETECTION_CLASSES[cls] || { label: label, color: '#ff4757' };
                // 生成一个模拟检测框（居中，固定大小，因为串口只传类别没有bbox）
                AppState.videoDetections = [{
                    class: cls,
                    label: meta.label,
                    color: meta.color,
                    confidence: 0.85,
                    x: 80, y: 80, w: 160, h: 160
                }];
                console.log('[K230/MQTT] 收到检测:', cls, '→', meta.label, '危险:', msg.k230_danger);
            } else {
                AppState.videoDetections = [];
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
            console.log('[语音] 期待接收', AppState.voiceSegmentCount, '段音频');
            return;
        }

        // --- 语音分段 PCM ---
        if (topic.startsWith('blindstick/voice/pcm/')) {
            const segIdx = parseInt(topic.split('/').pop());
            AppState.voiceSegments[segIdx] = new Uint8Array(payload);
            AppState.voiceSegmentReceived++;
            console.log('[语音] 收到第', segIdx, '段，共', AppState.voiceSegmentReceived, '段');

            // 如果收齐所有段
            if (AppState.voiceSegmentReceived >= AppState.voiceSegmentCount && AppState.voiceSegmentCount > 0) {
                // 拼接所有段
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
                console.log('[语音] 音频拼接完成', totalLen, '字节');
                // 识别并导航
                handleVoiceNavigation(fullPcm);
                AppState.voiceSegmentCount = 0;
            }
            return;
        }

        // --- TTS 音频（来自 ESP32 通过 MQTT 代理的 TTS 结果）---
        // 注意：所有语音由ESP32硬件功放播放，前端只记录日志不播放
        if (topic === MQTT_CONFIG.topics.ttsAudio) {
            const audioLen = payload.length;
            console.log('[TTS] ESP32播放完成通知，音频大小:', audioLen, '字节');
            // 前端不播放声音，只做记录
            return;
        }

        // --- TTS 请求（ESP32通过MQTT代理请求TTS） ---
        if (topic === MQTT_CONFIG.topics.ttsReq) {
            try {
                const msg = JSON.parse(payload.toString());
                console.log('[TTS-MQTT] 收到TTS请求:', msg.text);

                // 只要有text字段就调用TTS（ESP32发送的消息可能没有type字段）
                if (msg.text) {
                    // 根据type确定优先级，默认NORMAL(1)
                    let priority = msg.priority !== undefined ? msg.priority : TTS_PRIORITY.NORMAL;
                    if (msg.type === 'obstacle_alert' || msg.type === 'radar_alert') {
                        priority = TTS_PRIORITY.HIGH;
                    } else if (msg.type === 'navigation') {
                        priority = TTS_PRIORITY.LOW;
                    }
                    console.log(`[TTS-MQTT] 调用streamTTS: "${msg.text.substring(0, 30)}..." 优先级=${priority}`);
                    await streamTTS(msg.text, priority);
                } else {
                    console.log('[TTS-MQTT] 消息无text字段，忽略');
                }
            } catch (e) {
                console.error('[TTS-MQTT] 处理失败:', e);
            }
            return;
        }

        // --- 导航路线 ---
        if (topic === MQTT_CONFIG.topics.navSteps) {
            const msg = JSON.parse(payload.toString());
            if (msg.destination && msg.steps) {
                // 如果已经在导航中，且收到新的路线，算作路线调整
                if (AppState.reportData.navCount > 0 && !AppState.navJustStarted) {
                    AppState.reportData.detourCount++;
                    document.getElementById('detourCount').textContent = AppState.reportData.detourCount;
                    console.log('导航', `路线已调整：${msg.destination}`);
                }
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

function updateRadarCircles(front, frontLeft, frontRight, left, right) {
    const valFront = document.getElementById('valFront');
    const valFrontLeft = document.getElementById('valFrontLeft');
    const valFrontRight = document.getElementById('valFrontRight');
    const valLeft = document.getElementById('valLeft');
    const valRight = document.getElementById('valRight');
    const barFront = document.getElementById('barFront');
    const barFrontLeft = document.getElementById('barFrontLeft');
    const barFrontRight = document.getElementById('barFrontRight');
    const barLeft = document.getElementById('barLeft');
    const barRight = document.getElementById('barRight');

    // 调试：检查元素是否存在
    if (!valRight) console.warn('[雷达UI] valRight 元素不存在!');

    if (valFront) valFront.textContent = Math.round(front) + 'cm';
    if (valFrontLeft) valFrontLeft.textContent = Math.round(frontLeft) + 'cm';
    if (valFrontRight) valFrontRight.textContent = Math.round(frontRight) + 'cm';
    if (valLeft) valLeft.textContent = Math.round(left) + 'cm';
    if (valRight) valRight.textContent = Math.round(right) + 'cm';

    if (barFront) updateRadarBar(barFront, front);
    if (barFrontLeft) updateRadarBar(barFrontLeft, frontLeft);
    if (barFrontRight) updateRadarBar(barFrontRight, frontRight);
    if (barLeft) updateRadarBar(barLeft, left);
    if (barRight) updateRadarBar(barRight, right);

    // 调试：确认更新后的值
    console.log('[雷达UI更新] 右侧显示:', valRight ? valRight.textContent : 'N/A');
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
}

// ================= 地图 =================
let map, pathPolyline, marker;
function initMap() {
    const mapEl = document.getElementById('map');
    if (!mapEl) return;
    map = L.map('map', { zoomControl: false }).setView(AppState.gpsCenter, 15);
    L.tileLayer('https://webrd0{s}.is.autonavi.com/appmaptile?lang=zh_cn&size=1&scale=1&style=8&x={x}&y={y}&z={z}', {
        subdomains: ['1', '2', '3', '4'], attribution: '© 高德地图'
    }).addTo(map);
    pathPolyline = L.polyline([], { color: '#2563eb', weight: 4 }).addTo(map);
    marker = L.circleMarker(AppState.gpsCenter, { color: '#2563eb', radius: 6, fillColor: '#fff', fillOpacity: 1, weight: 3 }).addTo(map);
}

function updateGPS(lng, lat, speed) {
    const pos = [lat, lng];
    if (marker) marker.setLatLng(pos);
    if (pathPolyline) pathPolyline.addLatLng(pos);
    if (map) map.setView(pos);
    const lngEl = document.getElementById('lng');
    const latEl = document.getElementById('lat');
    const speedEl = document.getElementById('speed');
    if (lngEl) lngEl.textContent = lng.toFixed(4);
    if (latEl) latEl.textContent = lat.toFixed(4);
    if (speedEl) speedEl.textContent = (speed || 0).toFixed(1);
    // 计算里程
    if (AppState.lastGpsPos) {
        const dist = calcDistance(AppState.lastGpsPos.lat, AppState.lastGpsPos.lng, lat, lng);
        if (dist > 1 && dist < 100) { // 过滤掉跳变和静止
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
    // 更新GPS状态栏
    updateModuleStatus({ gps: count > 0 });
}

// ================= 导航记录 =================
function addNavHistory(destination, steps) {
    const el = document.getElementById('navHistoryList');
    if (!el) return;
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
    connectMQTT();        // ← MQTT 替代 WebSocket
    loadHomeCitySettings(); // 加载常住地设置
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

/**
 * 从文本中提取目的地（高级版，自动屏蔽非目的地词汇）
 * @param {string} text - 识别文本
 * @returns {string|null} - 提取的目的地或null
 */
function extractDestinationAdvanced(text) {
    if (!text || text.length < 2) return null;

    console.log('[目的地提取] 原始文本:', text);

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
        console.log('[目的地提取] 未找到导航触发词');
        return null;
    }

    // 步骤2：提取触发词后的内容
    let destination = text.substring(triggerIndex + matchedTrigger.length).trim();
    console.log('[目的地提取] 触发词后内容:', destination);

    // 步骤3：去除标点符号
    destination = destination.replace(/[，。？！.,?!；：""''（）()【】\[\]{}]/g, ' ');

    // 步骤4：按空格分割，取第一个非空部分
    const parts = destination.split(/\s+/).filter(p => p.length > 0);
    if (parts.length === 0) {
        console.log('[目的地提取] 触发词后无内容');
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
        // 去除开头的过滤词
        if (filtered.startsWith(word)) {
            filtered = filtered.substring(word.length);
        }
        // 去除结尾的过滤词
        if (filtered.endsWith(word)) {
            filtered = filtered.substring(0, filtered.length - word.length);
        }
    }

    filtered = filtered.trim();

    // 步骤6：验证目的地有效性
    if (filtered.length < 2) {
        console.log('[目的地提取] 过滤后内容太短:', filtered);
        return null;
    }

    // 不能全是数字或标点
    if (/^[\d\s\p{P}]+$/u.test(filtered)) {
        console.log('[目的地提取] 无效内容（纯数字/标点）:', filtered);
        return null;
    }

    console.log('[目的地提取] 最终目的地:', filtered);
    return filtered;
}

// ================= 新增：搜索最近目的地功能 =================

/**
 * 搜索最近的目的地（返回距离用户最近的结果）
 * @param {string} keyword - 目的地关键词
 * @param {number} currentLat - 当前纬度
 * @param {number} currentLng - 当前经度
 * @returns {Promise<Object|null>} - 最近的地点信息或null
 */
async function searchNearestDestination(keyword, currentLat, currentLng) {
    try {
        // 使用百度地点检索API搜索多个结果
        const searchUrl = `https://api.map.baidu.com/place/v2/search?query=${encodeURIComponent(keyword)}&region=${encodeURIComponent(API_CONFIG.homeCity)}&output=json&ak=${API_CONFIG.baiduMapAk}&page_size=10`;

        const searchRes = await fetch(searchUrl);
        const searchData = await searchRes.json();

        if (searchData.status !== 0 || !searchData.results || searchData.results.length === 0) {
            console.log('[最近目的地] 未找到结果');
            return null;
        }

        console.log('[最近目的地] 找到', searchData.results.length, '个结果');

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

            place._distance = distance; // 保存距离

            console.log(`[最近目的地] ${place.name}: ${Math.round(distance)}米`);

            if (distance < minDistance) {
                minDistance = distance;
                nearest = place;
            }
        }

        if (nearest) {
            console.log('[最近目的地] 最近的是:', nearest.name, '距离', Math.round(minDistance), '米');
        }

        return nearest;

    } catch (e) {
        console.error('[最近目的地] 搜索失败:', e);
        return null;
    }
}

/**
 * 规划到最近目的地的步行路线
 * @param {string} destination - 目的地关键词
 * @param {number} currentLat - 当前纬度
 * @param {number} currentLng - 当前经度
 * @returns {Promise<Object|null>} - 路线信息或null
 */
async function planRouteToNearest(destination, currentLat, currentLng) {
    // 搜索最近的目的地
    const nearestPlace = await searchNearestDestination(destination, currentLat, currentLng);

    if (!nearestPlace) {
        console.log('[路线规划] 未找到目的地');
        return null;
    }

    const distance = nearestPlace._distance || 0;

    // 检查距离是否太远
    if (distance > MAX_NAVIGATION_DISTANCE) {
        console.log('[路线规划] 距离太远:', Math.round(distance), '米');
        return {
            tooFar: true,
            distance: distance,
            destination: nearestPlace.name,
            message: `目的地${nearestPlace.name}距离您${Math.round(distance / 1000)}公里，距离太远`
        };
    }

    // 获取步行路线
    try {
        const origin = currentLat && currentLng ? `${currentLat},${currentLng}` : '30.229320,115.063977';
        const destLat = nearestPlace.location.lat;
        const destLng = nearestPlace.location.lng;

        const routeUrl = `https://api.map.baidu.com/directionlite/v1/walking?origin=${origin}&destination=${destLat},${destLng}&ak=${API_CONFIG.baiduMapAk}`;

        const routeRes = await fetch(routeUrl);
        const routeData = await routeRes.json();

        if (routeData.status !== 0 || !routeData.result || !routeData.result.routes || routeData.result.routes.length === 0) {
            console.error('[路线规划] 路线规划失败:', routeData);
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

// ================= 新增：开机播报（发送文本给ESP32功放播放）====================

/**
 * 开机播报 - 发送文本给ESP32，由ESP32功放播放
 */
async function playStartupSound() {
    console.log('[开机播报] 发送启动文本给ESP32');
    console.log('系统', '设备启动成功');

    const startupText = '导盲杖系统启动成功，欢迎使用';
    // 通过MQTT发送文本给ESP32，让ESP32自己合成语音并由功放播放
    if (mqttClient && AppState.mqttConnected) {
        const msg = JSON.stringify({
            type: 'tts_request',
            text: startupText
        });
        mqttClient.publish(MQTT_CONFIG.topics.ttsReq, msg);
        console.log('[开机播报] 已发送文本给ESP32:', startupText);
    } else {
        console.error('[开机播报] MQTT未连接');
    }
}

// ================= 新增：障碍物检测（只发送文本给ESP32播放）====================

/**
 * 处理五向雷达障碍物检测 - 只发送文本，由ESP32功放播放
 * @param {Object} radarData - 雷达数据 {front, frontLeft, frontRight, left, right}
 */
async function handleObstacleDetection(radarData) {
    const { front, frontLeft, frontRight, left, right } = radarData;
    const OBSTACLE_THRESHOLD = 180; // 障碍物告警阈值（厘米）

    // 找出最近的障碍物
    const distances = [
        { dist: front, dir: '正前方' },
        { dist: frontLeft, dir: '左前方' },
        { dist: frontRight, dir: '右前方' },
        { dist: left, dir: '左侧' },
        { dist: right, dir: '右侧' }
    ];

    let minDist = Infinity;
    let minDir = '';

    for (const item of distances) {
        if (item.dist < minDist) {
            minDist = item.dist;
            minDir = item.dir;
        }
    }

    // 如果最近障碍物在阈值内，且状态变化，则发送播报请求给ESP32
    if (minDist < OBSTACLE_THRESHOLD && !AppState.lastObstacleState) {
        AppState.reportData.obstacleCount++;
        document.getElementById('obstacleCount').textContent = AppState.reportData.obstacleCount;
        AppState.lastObstacleState = true;

        // 发送文本给ESP32，由ESP32自己合成并播放
        await announceObstacleWithDistance(Math.round(minDist), minDir);

    } else if (minDist >= 200) {
        AppState.lastObstacleState = false;
    }
}

/**
 * 发送障碍物告警文本给ESP32（由ESP32功放播放）
 * @param {number} distance - 障碍物距离（厘米）
 * @param {string} direction - 方向（如'前方'、'左前方'等）
 */
async function announceObstacleWithDistance(distance, direction = '前方') {
    console.log('[障碍物告警] 发送文本给ESP32:', direction, distance, 'cm');

    let alertText = '';

    // 根据距离生成不同的告警文本
    if (distance < 50) {
        alertText = `${direction}${distance}厘米有障碍物，请立即避让！`;
    } else if (distance < 100) {
        alertText = `${direction}${distance}厘米有障碍物，请注意避让`;
    } else if (distance < 200) {
        alertText = `${direction}${distance}厘米有障碍物`;
    } else {
        alertText = `${direction}${Math.round(distance / 100)}米处有障碍物`;
    }

    console.log('障碍物', alertText);

    // 通过MQTT发送文本给ESP32，让ESP32自己合成语音并由功放播放
    if (mqttClient && AppState.mqttConnected) {
        const msg = JSON.stringify({
            type: 'obstacle_alert',
            text: alertText,
            distance: distance,
            direction: direction
        });
        mqttClient.publish(MQTT_CONFIG.topics.ttsReq, msg);
        console.log('[障碍物告警] 已发送文本给ESP32:', alertText);
    } else {
        console.error('[障碍物告警] MQTT未连接');
    }
}

// ================= 新增：语音导航处理（改进版 - ESP32播放）====================

/**
 * 处理语音导航指令（改进版）- 由ESP32功放播放语音
 * @param {string} text - 识别的语音文本
 */
async function handleVoiceNavigationAdvanced(text) {
    console.log('[语音导航] 收到文本:', text);
    console.log('语音', `识别: ${text}`);

    // 提取目的地
    const destination = extractDestinationAdvanced(text);

    if (!destination) {
        console.log('[语音导航] 未提取到有效目的地');
        // 发送文本给ESP32，由ESP32播放提示音
        await baiduTTS('请说出具体地点，例如带我去天安门');
        return;
    }

    console.log('[语音导航] 目的地:', destination);
    console.log('导航', `目的地: ${destination}`);

    const currentPos = AppState.lastGpsPos;

    // 使用新的路线规划函数
    const route = await planRouteToNearest(destination, currentPos?.lat, currentPos?.lng);

    if (!route) {
        console.log('[语音导航] 路线规划失败');
        // 发送文本给ESP32，由ESP32播放提示音
        await baiduTTS('抱歉，没有找到该地点的路线');
        return;
    }

    // 检查距离是否太远
    if (route.tooFar) {
        console.log('[语音导航]', route.message);
        // 发送文本给ESP32，由ESP32播放提示音
        await baiduTTS(route.message + '，请重新选择较近的地点');
        console.log('导航', route.message);
        return;
    }

    // 发送导航路线
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
        console.log('[语音导航] 导航路线已发送:', route.steps.length, '步');
        console.log('导航', `开始导航到 ${route.destination}，共${route.steps.length}步，${Math.round(route.distance)}米`);
    }

    // 发送导航播报文本给ESP32，由ESP32功放播放
    const firstStep = route.steps[0] || '开始导航';
    const ttsText = `开始导航到${route.destination}，全程${Math.round(route.distance)}米，预计${Math.round(route.duration / 60)}分钟，${firstStep}`;
    await baiduTTS(ttsText);

    updateNavigationSteps(navMsg);
    addNavHistory(route.destination, route.steps);
}

// ================= 新增：初始化时播放开机播报 =================

// 导出函数供外部使用（如果需要）
if (typeof window !== 'undefined') {
    window.extractDestinationAdvanced = extractDestinationAdvanced;
    window.planRouteToNearest = planRouteToNearest;
    window.playStartupSound = playStartupSound;
    window.announceObstacleWithDistance = announceObstacleWithDistance;
    window.handleVoiceNavigationAdvanced = handleVoiceNavigationAdvanced;
}

// ================= 新增：常住地设置功能 =================

/**
 * 打开设置弹窗
 */
function openSettings() {
    const modal = document.getElementById('settingsModal');
    const currentCityDiv = document.getElementById('currentHomeCity');
    const homeCityInput = document.getElementById('homeCityInput');

    // 加载当前常住地
    const savedCity = localStorage.getItem('homeCity') || API_CONFIG.homeCity;
    if (currentCityDiv) {
        currentCityDiv.textContent = savedCity;
    }
    if (homeCityInput) {
        homeCityInput.value = '';
        homeCityInput.placeholder = `例如：${savedCity}`;
    }

    modal.style.display = 'flex';
    console.log('[设置] 打开设置弹窗，当前常住地:', savedCity);
}

/**
 * 关闭设置弹窗
 */
function closeSettings() {
    const modal = document.getElementById('settingsModal');
    modal.style.display = 'none';
}

/**
 * 保存常住地设置
 */
function saveSettings() {
    const homeCityInput = document.getElementById('homeCityInput');
    const newCity = homeCityInput.value.trim();

    if (!newCity) {
        showToast('请输入常住地城市名称');
        return;
    }

    // 保存到 localStorage
    localStorage.setItem('homeCity', newCity);

    // 更新 API_CONFIG
    API_CONFIG.homeCity = newCity;
    AppState.config.homeCity = newCity;

    // 通过 MQTT 发送给 ESP32
    if (mqttClient && AppState.mqttConnected) {
        const msg = JSON.stringify({
            type: 'home_city_update',
            city: newCity,
            ts: Date.now()
        });
        mqttClient.publish('blindstick/config/home_city', msg);
        console.log('[设置] 常住地已发送给ESP32:', newCity);
    }

    // 更新显示
    const currentCityDiv = document.getElementById('currentHomeCity');
    if (currentCityDiv) {
        currentCityDiv.textContent = newCity;
    }

    showToast(`常住地已设置为：${newCity}`);
    closeSettings();

    console.log('[设置] 常住地已保存:', newCity);
}

/**
 * 加载常住地设置（从 localStorage）
 */
function loadHomeCitySettings() {
    const savedCity = localStorage.getItem('homeCity');
    if (savedCity) {
        API_CONFIG.homeCity = savedCity;
        AppState.config.homeCity = savedCity;
        console.log('[设置] 已加载常住地:', savedCity);
    } else {
        console.log('[设置] 使用默认常住地:', API_CONFIG.homeCity);
    }
}
