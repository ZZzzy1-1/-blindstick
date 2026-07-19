#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <SoftwareSerial.h>  // 软串口库
#include <PubSubClient.h>
#include <driver/i2s.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_heap_caps.h>

// ==================== 网络参数 ====================
const char* WIFI_SSID     = "ZZY";
const char* WIFI_PASSWORD = "zzy060630";

// ==================== MQTT 参数（EMQX Cloud） ====================
const char* MQTT_BROKER   = "u72a7838.ala.asia-southeast1.emqxsl.com";
const int   MQTT_PORT     = 8883;
const char* MQTT_USER     = "blindstick";
const char* MQTT_PASSWORD = "2026";
const char* MQTT_CLIENT_ID = "blindstick_esp32_001";
const char* MQTT_TOPIC_SENSORS   = "blindstick/sensors";
const char* MQTT_TOPIC_TTS_REQ   = "blindstick/tts/request";
const char* MQTT_TOPIC_TTS_AUDIO = "blindstick/tts/audio";
const char* MQTT_TOPIC_NAV_STEPS = "blindstick/nav/steps";

WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

// ==================== 流式TTS配置（新版）====================
// 优先级定义
#define PRIO_LOW     0   // 导航
#define PRIO_NORMAL  1   // 对话
#define PRIO_HIGH    2   // 雷达告警

// 流式播放状态
volatile bool stream_playing = false;           // 是否正在播放
volatile int  stream_priority = 0;              // 当前播放优先级
volatile unsigned long stream_session_id = 0;   // 当前会话ID

// 音频格式
#define AUDIO_FORMAT_PCM_16K  0  // PCM 16kHz 16bit
#define AUDIO_FORMAT_WAV      1  // WAV格式

// 缓冲区配置（用于流式接收）
#define STREAM_BUF_SIZE  8192   // 8KB流式缓冲区
uint8_t* stream_buffer = NULL;
volatile int stream_buf_used = 0;

// 旧的TTS缓冲区（兼容旧版）
#define TTS_AUDIO_BUF_SIZE  (120 * 1024)
volatile uint8_t tts_rx_buf[TTS_AUDIO_BUF_SIZE];
volatile int     tts_rx_len = 0;
volatile bool    tts_rx_ready = false;
volatile unsigned long tts_rx_start = 0;
#define TTS_RX_TIMEOUT_MS  5000

// ==================== 百度语音 API 配置（仅用于语音识别）====================
const String BAIDU_API_KEY        = "Xbxnhkwb2sxtB6HbH5BUTlUG";
const String BAIDU_SECRET_KEY     = "Tw485P2BFGpPu8WeOVP6hy4S1BHqG4ON";

// ==================== 语音识别和导航配置 ====================
#define MAX_NAVIGATION_DISTANCE 10000  // 最大导航距离10公里（米）

// 导航触发词
const char* NAV_TRIGGERS[] = {"带我去", "我要去", "我想去", "导航到", "我去", "去", "到"};
const int NAV_TRIGGER_COUNT = 7;

// 需要过滤的非目的地词汇
const char* FILTER_WORDS[] = {
    "的", "了", "在", "是", "我", "有", "和", "就", "不", "人", "都", "一", "一个",
    "一下", "那个", "这个", "那里", "这里", "吧", "啊", "呢", "吗", "哦", "嗯",
    "请", "把", "给", "跟", "对", "向", "从", "让", "被", "比",
    "附近", "周围", "旁边", "对面"
};
const int FILTER_WORD_COUNT = 34;

// ==================== 雷达与电机引脚 ====================
#define RADAR_RX_PIN    18    // YDLIDAR X2 TX → ESP32 GPIO18 (UART RX)
#define RADAR_M_CTR_PIN 8     // YDLIDAR X2 电机控制 → ESP32 GPIO8（启动雷达电机）

#define MOTOR_IN1       12    // TB6612 AIN1 → GPIO12
#define MOTOR_IN2       11    // TB6612 AIN2 → GPIO11
#define MOTOR_PWM       10    // TB6612 PWMA → GPIO10

// ==================== YDLIDAR X2 启动命令 ====================
static const uint8_t YDLIDAR_CMD_START[] = { 0xA5, 0x60, 0x00, 0x60, 0x01, 0x00, 0x60, 0xE8 };
static const uint8_t YDLIDAR_CMD_STOP[]  = { 0xA5, 0x65, 0x00, 0x65, 0x01, 0x00, 0x65, 0x1B };
static const uint8_t YDLIDAR_CMD_RESET[] = { 0xA5, 0x40, 0x00, 0x40, 0x01, 0x00, 0x40, 0x97 };

#define GPS_RX_PIN      16
#define GPS_TX_PIN      17
HardwareSerial gpsSerial(2);

#define RECORD_BUTTON_PIN  0

// ==================== I2S麦克风引脚 (INMP441) ====================
// 实际硬件接线：
// VDD → 3.3V
// WS  → GPIO2  (LRCK)
// SCK → GPIO1  (BCLK)
// SD  → GPIO42  (MIC_IN)
// GND → GND
// L/R → GND (接地=左声道)
#define I2S_WS_PIN      2   // LRCK
#define I2S_SCK_PIN     1   // BCLK
#define I2S_SD_PIN      42   // MIC_IN
#define I2S_PORT        I2S_NUM_0

// ==================== I2S扬声器引脚 (MAX98357) ====================
#define I2S_BCK_PIN     4  // SPK_BCLK
#define I2S_WS_OUT_PIN  5  // SPK_LRCK
#define I2S_DATA_PIN    6  // SPK_OUT
#define I2S_PORT_OUT    I2S_NUM_1

#define VOLUME_GAIN     0.85  // 音量增益 (0.0-1.0)，增大音量


// 播放开机提示音 - 短促的"滴"一声
void playStartupVoice() {
    Serial.println("[开机] 播放启动提示音...");

    const int sample_rate = 16000;
    const int beep_duration = 80;  // 80ms短促提示
    const int freq = 1500;         // 1500Hz频率

    int16_t beep_buffer[1280];  // 80ms @ 16kHz
    for (int i = 0; i < 1280; i++) {
        float t = (float)i / sample_rate;
        float sample = sin(2 * PI * freq * t) * 8000.0f;
        // 淡入淡出
        if (i < 80) sample *= (i / 80.0f);
        if (i > 1200) sample *= ((1280 - i) / 80.0f);
        beep_buffer[i] = (int16_t)sample;
    }

    i2s_zero_dma_buffer(I2S_PORT_OUT);
    i2s_write(I2S_PORT_OUT, beep_buffer, sizeof(beep_buffer), NULL, portMAX_DELAY);
    delay(beep_duration);
    i2s_zero_dma_buffer(I2S_PORT_OUT);

    Serial.println("[开机] 启动完成");
}

// ==================== 音频采样参数 ====================
#define SAMPLE_RATE     16000  // 16kHz 采样率

// ==================== 函数声明 ====================
String urlEncode(const char* str);
float calcDistance(float lat1, float lng1, float lat2, float lng2);

// 流式TTS相关函数
void initStreamingTTS();
void handleStreamControl(const char* payload, int length);
void handleStreamAudio(const char* topic, byte* payload, unsigned int length);
void handleTTSUrl(const char* payload, int length);  // URL方式下载并播放TTS
void playPcmData(uint8_t* data, int len);
void stopCurrentPlayback();
const char* getPrioName(int p);

// 流式语音识别相关
void VoiceRecognitionTask(void* pvParameters);
String doVoiceRecognition();
String getBaiduToken();
void handleVoiceCommand(const char* text);
String base64Encode(const uint8_t* data, size_t len);  // Base64编码

// ==================== 工具函数实现 ====================

/**
 * URL编码
 */
String urlEncode(const char* str) {
    String encoded = "";
    char c;
    for (int i = 0; str[i] != '\0'; i++) {
        c = str[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else {
            char buf[4];
            sprintf(buf, "%%%02X", c);
            encoded += buf;
        }
    }
    return encoded;
}

/**
 * 计算两点间距离（米）
 */
float calcDistance(float lat1, float lng1, float lat2, float lng2) {
    const float R = 6371000; // 地球半径（米）
    float dLat = (lat2 - lat1) * PI / 180.0;
    float dLng = (lng2 - lng1) * PI / 180.0;
    float a = sin(dLat/2) * sin(dLat/2) +
              cos(lat1 * PI / 180.0) * cos(lat2 * PI / 180.0) *
              sin(dLng/2) * sin(dLng/2);
    float c = 2 * atan2(sqrt(a), sqrt(1-a));
    return R * c;
}

/**
 * 获取优先级名称
 */
const char* getPrioName(int p) {
    switch(p) {
        case PRIO_HIGH: return "高(雷达)";
        case PRIO_NORMAL: return "中(对话)";
        case PRIO_LOW: return "低(导航)";
        default: return "未知";
    }
}

// ==================== 避障阈值 ====================
#define ALERT_DIST_CM       180.0
#define FRONT_CRITICAL_CM   180.0
#define SIDE_WARNING_CM     180.0
#define AVOID_TURN_HOLD_MS 2000

// ==================== 雷达角度扇区 ====================
#define ANG_FRONT_MIN  350
#define ANG_FRONT_MAX  10
#define ANG_LEFT_MIN   70
#define ANG_LEFT_MAX   110
#define ANG_RIGHT_MIN  250
#define ANG_RIGHT_MAX  290

#define UPLOAD_INTERVAL_MS  200
#define STEER_MAX_PWM  255

// ==================== TTS 配置 ====================
enum TTS_Priority {
    TTS_PRIORITY_LOW = 0,
    TTS_PRIORITY_NORMAL = 1,
    TTS_PRIORITY_HIGH = 2
};

SemaphoreHandle_t audioMutex = NULL;

// ==================== 全局状态变量 ====================
String nav_steps[10];
volatile int nav_total_steps = 0;
volatile int current_step_idx = 0;
volatile int current_progress = 0;
volatile bool nav_active = false;

volatile bool  is_blocked  = false;
volatile bool  is_ai_talking = false;
volatile bool  is_tts_requesting = false;  // TTS请求状态标志，防止重复发送
unsigned long  tts_request_start_time = 0;

// TTS请求标志互斥锁（防止竞态条件）
SemaphoreHandle_t ttsRequestMutex = NULL;

// TTS请求标志的安全访问函数
inline void setTTSRequesting(bool value) {
    if (ttsRequestMutex != NULL) {
        if (xSemaphoreTake(ttsRequestMutex, portMAX_DELAY) == pdTRUE) {
            is_tts_requesting = value;
            if (value) {
                tts_request_start_time = millis();
            }
            xSemaphoreGive(ttsRequestMutex);
        }
    } else {
        // 互斥锁未初始化时直接赋值（兼容旧代码）
        is_tts_requesting = value;
        if (value) {
            tts_request_start_time = millis();
        }
    }
}

inline bool getTTSRequesting() {
    bool value = false;
    if (ttsRequestMutex != NULL) {
        if (xSemaphoreTake(ttsRequestMutex, portMAX_DELAY) == pdTRUE) {
            value = is_tts_requesting;
            xSemaphoreGive(ttsRequestMutex);
        }
    } else {
        value = is_tts_requesting;
    }
    return value;
}

int last_motor_pwm = 0;
String last_motor_dir = "stop";

float gps_lat = 0.0;
float gps_lng = 0.0;
float gps_speed = 0.0;
int   gps_heading = 0;
int   gps_satellites = 0;

// 常住地设置（默认黄石市，可通过MQTT更新）
String home_city = "黄石市";

// 开机语音播报标志（只播报一次）- 使用RTC内存保持，深度睡眠后也能记住
RTC_DATA_ATTR static bool startup_announced_rtc = false;
volatile bool startup_announced = false;  // 运行时标志，用于防止同一运行周期内重复

enum LidarState { WAIT_HEADER_AA, WAIT_HEADER_55, READ_CT, READ_LSN, READ_PAYLOAD };
volatile LidarState lidar_state = WAIT_HEADER_AA;
volatile uint8_t packet_ct = 0, packet_lsn = 0;
volatile uint8_t payload_buf[128];
volatile uint8_t payload_idx = 0, payload_expected = 0;

TaskHandle_t RadarTaskHandle = NULL;
TaskHandle_t NavTaskHandle = NULL;
TaskHandle_t VoiceTaskHandle = NULL;
TaskHandle_t TTSPlayerTaskHandle = NULL;

// ==================== 五向雷达 + EMA平滑 ====================
#define NUM_DIR     3
#define SMOOTH_A    0.50f

volatile float dir_raw[NUM_DIR] = {400.0f, 400.0f, 400.0f};
volatile float dir_smt[NUM_DIR] = {400.0f, 400.0f, 400.0f};

// ==================== 智能避障参数（用户算法）====================
#define STEER_MAX_PWM 230
#define STEER_SLOW_PWM 180

#define FRONT_CRITICAL 60.0
#define SIDE_WARNING   50.0

#define ANG_FRONT_MIN  330
#define ANG_FRONT_MAX  30
#define ANG_LEFT_MIN   60
#define ANG_LEFT_MAX   120
#define ANG_RIGHT_MIN  240
#define ANG_RIGHT_MAX  300

float frontDist = 200.0;
float leftDist  = 200.0;
float rightDist = 200.0;

// ==================== 电机控制 ====================
// ==================== 电机控制（正数右转，负数左转）====================
void motorControl(int steerPower) {
    int safePower = constrain(steerPower, -STEER_MAX_PWM, STEER_MAX_PWM);

    if (safePower > 15) {
        // 👉 产生向右的动力
        digitalWrite(MOTOR_IN1, HIGH);
        digitalWrite(MOTOR_IN2, LOW);
        analogWrite(MOTOR_PWM, safePower);
    }
    else if (safePower < -15) {
        // 👈 产生向左的动力
        digitalWrite(MOTOR_IN1, LOW);
        digitalWrite(MOTOR_IN2, HIGH);
        analogWrite(MOTOR_PWM, abs(safePower));
    }
    else {
        // 🛑 危机解除，释放电机滑行
        digitalWrite(MOTOR_IN1, LOW);
        digitalWrite(MOTOR_IN2, LOW);
        analogWrite(MOTOR_PWM, 0);
    }
}

// ==================== 避障决策（智能左右权重避障算法）====================
void smartAvoid() {
    float leftForce = 0.0;
    float rightForce = 0.0;

    // --- 步骤 A：计算左侧物体的【向右排斥力】 ---
    if (leftDist < SIDE_WARNING) {
        leftForce = (SIDE_WARNING - leftDist) * 4.0;
    }

    // --- 步骤 B：计算右侧物体的【向左排斥力】 ---
    if (rightDist < SIDE_WARNING) {
        rightForce = (SIDE_WARNING - rightDist) * 4.0;
    }

    // --- 步骤 C：综合大局进行合力判断 ---

    // 情况 1：正前方小于60cm，紧急全速避障
    if (frontDist < FRONT_CRITICAL) {
        Serial.printf("🚨 前方紧急(%.1fcm)！全速闪避！", frontDist);

        if (leftDist > rightDist) {
            // 左边更空，满功率左转
            Serial.println("👈 左边更空，全速向左闪避！");
            motorControl(-STEER_MAX_PWM);
        } else {
            // 右边更空，满功率右转
            Serial.println("👉 右边更空，全速向右闪避！");
            motorControl(STEER_MAX_PWM);
        }
        return; // 紧急模式直接返回，不走低速逻辑
    }

    // 情况 2：前方安全，仅侧边有障碍物 -> 低速微调
    if (leftForce > 0 || rightForce > 0) {
        float netSteerSignal = leftForce - rightForce;

        // 关键：把合力映射到低速区间 STEER_SLOW_PWM，避免猛打方向
        // 先算出原始最大可能推力：SIDE_WARNING * 4 = 320
        // 按比例缩放到低速70以内
        float scaleRatio = STEER_SLOW_PWM / (SIDE_WARNING * 4.0f);
        int slowSteer = netSteerSignal * scaleRatio;

        Serial.printf("⚠️ 侧边预警(低速) -> 左距:%.1fcm, 右距:%.1fcm | 输出低速:%d\n",
                      leftDist, rightDist, slowSteer);

        motorControl(slowSteer);
    }
    // 情况 3：无障碍物，停机
    else {
        motorControl(0);
    }
}

// ==================== 雷达处理 ====================

void processRadarPacket() {
    uint16_t fsa = payload_buf[0] | (payload_buf[1] << 8);
    uint16_t lsa = payload_buf[2] | (payload_buf[3] << 8);
    float angleFSA = (fsa >> 1) / 64.0f;
    float angleLSA = (lsa >> 1) / 64.0f;
    float diffAngle = angleLSA - angleFSA;
    if (diffAngle < 0) diffAngle += 360.0f;

    // 重置三个方向的距离
    if (angleFSA >= ANG_FRONT_MIN || angleFSA <= ANG_FRONT_MAX) {
        frontDist = 200.0;
    }
    else if (angleFSA >= ANG_LEFT_MIN && angleFSA <= ANG_LEFT_MAX) {
        leftDist = 200.0;  // 修正：左方区域对应leftDist
    }
    else if (angleFSA >= ANG_RIGHT_MIN && angleFSA <= ANG_RIGHT_MAX) {
        rightDist = 200.0;  // 修正：右方区域对应rightDist
    }

    for (int i = 0; i < packet_lsn; i++) {
        uint16_t si = payload_buf[6 + i * 2] | (payload_buf[6 + i * 2 + 1] << 8);
        float distanceMm = si / 4.0f;

        if (distanceMm > 50.0f && distanceMm < 6000.0f) {
            float cm = distanceMm / 10.0f;
            float currentAngle = angleFSA;
            if (packet_lsn > 1) currentAngle += (diffAngle / (packet_lsn - 1)) * i;
            if (currentAngle >= 360.0f) currentAngle -= 360.0f;

            if (currentAngle >= ANG_FRONT_MIN || currentAngle <= ANG_FRONT_MAX) {
                if (cm < frontDist) frontDist = cm;
            }
            else if (currentAngle >= ANG_LEFT_MIN && currentAngle <= ANG_LEFT_MAX) {
                if (cm < leftDist) leftDist = cm;  // 修正：左方区域存入leftDist
            }
            else if (currentAngle >= ANG_RIGHT_MIN && currentAngle <= ANG_RIGHT_MAX) {
                if (cm < rightDist) rightDist = cm;  // 修正：右方区域存入rightDist
            }
        }
    }

    // EMA平滑处理
    dir_smt[0] = SMOOTH_A * frontDist + (1.0f - SMOOTH_A) * dir_smt[0];
    dir_smt[1] = SMOOTH_A * leftDist + (1.0f - SMOOTH_A) * dir_smt[1];
    dir_smt[2] = SMOOTH_A * rightDist + (1.0f - SMOOTH_A) * dir_smt[2];

    // 同步回用户变量
    frontDist = dir_smt[0];
    leftDist = dir_smt[1];
    rightDist = dir_smt[2];
}

void parseGPSNMEA() {
    static char nmea[256];
    static uint8_t idx = 0;
    while (gpsSerial.available()) {
        char c = gpsSerial.read();
        if (c == '$') { idx = 0; nmea[idx++] = c; }
        else if (idx > 0 && idx < 255) {
            nmea[idx++] = c;
            if (c == '\n' || c == '\r') {
                nmea[idx] = '\0';
                // 解析 GGA 语句：位置 + 卫星数
                if (strstr(nmea, "GGA") != NULL) {
                    float lat_raw = 0, lng_raw = 0;
                    char ns = 'N', ew = 'E';
                    int fix = 0, sats = 0;
                    if (sscanf(nmea, "$%*[^,],%*[^,],%f,%c,%f,%c,%d,%d,", &lat_raw, &ns, &lng_raw, &ew, &fix, &sats) >= 6) {
                        gps_satellites = sats;
                        if (fix > 0 && lat_raw > 0.0f && lng_raw > 0.0f) {
                            float lat = lat_raw / 100.0f;
                            float lat_d = floor(lat);
                            lat = lat_d + (lat - lat_d) * 100.0f / 60.0f;
                            if (ns == 'S') lat = -lat;
                            float lng = lng_raw / 100.0f;
                            float lng_d = floor(lng);
                            lng = lng_d + (lng - lng_d) * 100.0f / 60.0f;
                            if (ew == 'W') lng = -lng;
                            gps_lat = lat; gps_lng = lng;
                        }
                    }
                }
                // 解析 RMC 语句：速度 + 航向
                else if (strstr(nmea, "RMC") != NULL) {
                    // RMC格式: $GPRMC,hhmmss.ss,A,lat,NS,lng,EW,speed_knots,course,ddmmyy,mag_var,mode
                    // 使用sscanf直接解析，跳过可选字段
                    char status = 'V';
                    float lat_dummy = 0, lng_dummy = 0, speed_knots = 0, course_dummy = 0;
                    char ns_dummy = 'N', ew_dummy = 'E';

                    // 解析前9个字段（到速度为止）
                    int parsed = sscanf(nmea, "$%*[^,],%*[^,],%c,%f,%c,%f,%c,%f,%f,",
                                        &status, &lat_dummy, &ns_dummy, &lng_dummy,
                                        &ew_dummy, &speed_knots, &course_dummy);

                    // 只要解析到速度字段（至少8个字段）且定位有效
                    if (parsed >= 6 && status == 'A' && speed_knots >= 0) {
                        // 节转 m/s: 1 节 = 0.514444 m/s
                        gps_speed = speed_knots * 0.514444f;
                    }
                }
                idx = 0;
            }
        }
    }
}

// ==================== MQTT 重连 ====================
void mqtt_reconnect() {
    // 配置MQTT客户端参数
    mqtt.setSocketTimeout(10);
    mqtt.setKeepAlive(60);
    mqtt.setBufferSize(131072);

    int retryCount = 0;
    while (!mqtt.connected()) {
        // 每次重试前重新配置TLS
        espClient.setInsecure();
        espClient.setHandshakeTimeout(12);

        // 确保WiFi连接状态正常
        if (WiFi.status() != WL_CONNECTED) {
            delay(1000);
            continue;
        }

        if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
            Serial.println("[MQTT] 已连接！");
            mqtt.subscribe(MQTT_TOPIC_TTS_AUDIO);
            mqtt.subscribe("blindstick/tts/control");
            mqtt.subscribe("blindstick/tts/stream/+");
            mqtt.subscribe("blindstick/tts/url");
            mqtt.subscribe(MQTT_TOPIC_NAV_STEPS);
            mqtt.subscribe(MQTT_TOPIC_TTS_REQ);
            mqtt.subscribe("blindstick/config/home_city");
            retryCount = 0;

            // 【开机语音】只在系统启动后的首次MQTT连接时播放一次本地音频
            if (!startup_announced && !startup_announced_rtc) {
                delay(200);
                playStartupVoice();
                startup_announced = true;
                startup_announced_rtc = true;
                Serial.println("[系统] 开机完成");
            }
        } else {
            retryCount++;
            if (retryCount >= 5) {
                retryCount = 0;
            }
            delay(5000);
        }
    }
}

// ==================== MQTT 消息回调（支持流式TTS）====================
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    // ===== 流式TTS控制消息处理（最高优先级）=====
    if (strcmp(topic, "blindstick/tts/control") == 0) {
        handleStreamControl((const char*)payload, length);
        return;
    }

    // ===== 流式TTS音频数据处理 =====
    if (strncmp(topic, "blindstick/tts/stream/", 22) == 0) {
        handleStreamAudio(topic, payload, length);
        return;
    }

    // ===== TTS URL处理（新方案：接收URL并下载）=====
    if (strcmp(topic, "blindstick/tts/url") == 0) {
        handleTTSUrl((const char*)payload, length);
        return;
    }

    // ===== 常住地设置处理 =====
    if (strcmp(topic, "blindstick/config/home_city") == 0) {
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, payload, length);
        if (!err && doc.containsKey("city")) {
            const char* new_city = doc["city"];
            home_city = String(new_city);

            // 播报确认
            StaticJsonDocument<256> ttsDoc;
            char confirmText[64];
            snprintf(confirmText, sizeof(confirmText), "常住地已设置为%s", home_city.c_str());
            ttsDoc["text"] = confirmText;
            ttsDoc["priority"] = PRIO_NORMAL;
            char buf[256];
            size_t len = serializeJson(ttsDoc, buf, sizeof(buf));
            mqtt.publish("blindstick/tts/request", buf, len);
        }
        return;
    }

    // 打印前 20 字节内容用于调试（仅非音频数据）
    if (length < 1000) {
        Serial.print("[MQTT] 内容: ");
        for (int i = 0; i < min(50, (int)length); i++) {
            Serial.printf("%c", payload[i]);
        }
        Serial.println();
    }

    // ===== 原有的TTS_AUDIO处理（兼容旧版）=====
    if (strcmp(topic, MQTT_TOPIC_TTS_AUDIO) == 0) {
        if (length < 1000 || length >= TTS_AUDIO_BUF_SIZE) {
            Serial.printf("[TTS] 音频长度无效: %d\n", length);
            return;
        }

        // 暂停语音识别
        if (VoiceTaskHandle != NULL) {
            vTaskSuspend(VoiceTaskHandle);
        }

        // 使用互斥锁保护音频缓冲区
        if (xSemaphoreTake(audioMutex, portMAX_DELAY) == pdTRUE) {
            // 分配内存并复制音频数据
            uint8_t* audio_buf = (uint8_t*)allocateBuffer(length);
            if (audio_buf == NULL) {
                Serial.println("[TTS] 内存分配失败，无法播放");
                xSemaphoreGive(audioMutex);
                return;
            }

            memcpy(audio_buf, payload, length);
            xSemaphoreGive(audioMutex);

            // 完整音频直接播放（跳过WAV头）
            int offset = 0;
            if (length > 44 && audio_buf[0] == 'R' && audio_buf[1] == 'I') {
                offset = 44;
            }
            playPcmData(audio_buf + offset, length - offset);
            free(audio_buf);

            // 重置TTS请求标志
            setTTSRequesting(false);

            // 恢复语音识别 - 确保在TTS完成后恢复
            if (VoiceTaskHandle != NULL) {
                eTaskState taskState = eTaskGetState(VoiceTaskHandle);
                if (taskState == eSuspended) {
                    vTaskResume(VoiceTaskHandle);
                }
            }
        }

    } else if (strcmp(topic, MQTT_TOPIC_NAV_STEPS) == 0) {
        StaticJsonDocument<4096> doc;
        DeserializationError err = deserializeJson(doc, payload, length);
        if (!err) {
            // 检查是否是停止导航指令
            if (doc["nav_active"] == false || doc["status"] == "stop") {
                nav_active = false;
                nav_total_steps = 0;
                current_step_idx = 0;
                current_progress = 0;
                Serial.println("[导航] 收到停止指令，导航已停止");
                // 播报导航停止
                StaticJsonDocument<256> ttsDoc;
                ttsDoc["text"] = "导航已停止";
                ttsDoc["priority"] = PRIO_NORMAL;
                char buf[256];
                size_t len = serializeJson(ttsDoc, buf, sizeof(buf));
                mqtt.publish("blindstick/tts/request", buf, len);
            }
            // 检查是否是新路线
            else if (doc["status"] == "ok" && doc["steps"]) {
                JsonArray steps = doc["steps"];
                nav_total_steps = 0;
                for (const char* step : steps) {
                    nav_steps[nav_total_steps] = String(step);
                    nav_total_steps++; if (nav_total_steps >= 10) break;
                }
                current_step_idx = 0; current_progress = 0; nav_active = true;
                Serial.printf("[导航] 开始: %s, %d步\n",
                    doc.containsKey("destination") ? doc["destination"].as<const char*>() : "未知",
                    nav_total_steps);
            }
        }

    } else if (strcmp(topic, MQTT_TOPIC_TTS_REQ) == 0) {
        // 【注意】ESP32不再转发TTS请求到代理服务器
        // 代理服务器直接订阅 blindstick/tts/request，无需ESP32转发
        // 这避免了MQTT消息循环问题
        // 如果需要本地处理TTS请求，可以在这里添加代码
    }
}

    // 全局变量
bool last_blocked = false;
unsigned long last_alert_time = 0;
float last_alert_dist = 0;
#define ALERT_INTERVAL_MS 8000        // 障碍物告警间隔 8 秒（避免过于频繁）
#define ALERT_DIST_CHANGE 30          // 距离变化超过30cm才重新播报

// 避障语音去重：记录上次播报的文本和时间
static String last_alert_text = "";
static unsigned long last_alert_text_time = 0;
#define ALERT_TEXT_DUPLICATE_MS 10000  // 10秒，相同文本不重复

// 【优化】TTS触发阈值
#define TTS_TRIGGER_DISTANCE_CM 60     // 距离小于60cm触发紧急播报
#define TTS_TRIGGER_COUNT 2            // 连续检测到2次触发（更快响应）

// ==================== 障碍物检测和播报（三向雷达版）====================
void checkObstacleAndAlert() {
    // 三向雷达: [0]=前方, [1]=左方, [2]=右方
    float f = dir_smt[0];
    float L = dir_smt[1];
    float R = dir_smt[2];

    // 阈值定义
    const float FRONT_ALERT_CM = 80.0f;
    const float SIDE_ALERT_CM = 50.0f;

    unsigned long now = millis();

    // 找出最近的障碍物
    float minDist = min(f, min(L, R));
    static int consecutiveAlerts = 0;

    // 判断哪个方向有障碍物
    bool has_obstacle = false;
    String alert_text = "";

    if (f < FRONT_ALERT_CM) {
        has_obstacle = true;
        if (L > R && L > SIDE_ALERT_CM) {
            alert_text = "前方有障碍物，请向左绕行";  // -> voice_left
        } else if (R >= L && R > SIDE_ALERT_CM) {
            alert_text = "前方有障碍物，请向右绕行";  // -> voice_right
        } else {
            alert_text = "前方有障碍物，请注意避让";  // -> voice_front
        }
    }
    else if (L < SIDE_ALERT_CM && L < R) {
        has_obstacle = true;
        if (R > SIDE_ALERT_CM && f > FRONT_ALERT_CM) {
            alert_text = "左方有障碍物，请向右绕行";  // -> voice_right
        } else {
            alert_text = "前方有障碍物，请注意避让";  // -> voice_front (默认)
        }
    }
    else if (R < SIDE_ALERT_CM) {
        has_obstacle = true;
        if (L > SIDE_ALERT_CM && f > FRONT_ALERT_CM) {
            alert_text = "右方有障碍物，请向左绕行";  // -> voice_left
        } else {
            alert_text = "前方有障碍物，请注意避让";  // -> voice_front (默认)
        }
    }

    // 连续检测计数
    if (has_obstacle) {
        consecutiveAlerts++;
    } else {
        consecutiveAlerts = 0;
    }

    // 触发条件检查
    if (has_obstacle && consecutiveAlerts >= TTS_TRIGGER_COUNT) {
        // 基础检查
        if (is_ai_talking || getTTSRequesting()) {
            return;
        }

        // 策略1：距离很近（<60cm）立即播报
        bool isUrgent = minDist < TTS_TRIGGER_DISTANCE_CM;

        // 策略2：普通情况间隔8秒
        bool timeOK = (now - last_alert_time >= ALERT_INTERVAL_MS);

        if (!isUrgent && !timeOK) {
            return;
        }

        // 去重检查
        if (alert_text == last_alert_text && (now - last_alert_text_time) < ALERT_TEXT_DUPLICATE_MS) {
            return;
        }

        // 雷达避障使用"嘀嘀嘀"提示音（不占用内存，无需网络）
        Serial.println("[障碍物] 播放嘀嘀嘀提示音");
        playAlertBeep();

        // 更新记录
        last_alert_time = now;
        last_alert_text = alert_text;
        last_alert_text_time = now;
        consecutiveAlerts = 0;
    }
}
// ==================== TTS请求超时时间 ====================
#define TTS_REQUEST_TIMEOUT_MS 10000

// ==================== 辅助函数：使用PSRAM或普通内存分配 ====================
void* allocateBuffer(size_t size) {
    if (ESP.getPsramSize() > 0) {
        return ps_malloc(size);
    } else {
        return malloc(size);
    }
}

static char json_buffer[2048];  // 扩大到2KB以容纳视觉检测数据

// ==================== 通过 MQTT 发布传感器数据 ====================
void publishSensorData() {
    if (!mqtt.connected()) return;

    // 使用更大的JSON缓冲区容纳视觉检测数据
    StaticJsonDocument<1024> doc;
    doc["device_id"] = "blind_stick_001";
    JsonObject radar = doc.createNestedObject("radar");
    // 三向雷达: [0]=前方, [1]=左方, [2]=右方
    radar["f"] = dir_smt[0];  // 前方
    radar["l"] = dir_smt[1];  // 左方
    radar["r"] = dir_smt[2];  // 右方
    doc["blocked"] = is_blocked;
    doc["nav"] = nav_active;
    JsonObject gps = doc.createNestedObject("gps");
    gps["lat"] = gps_lat;
    gps["lng"] = gps_lng;
    gps["satellites"] = gps_satellites;  // 统一字段名与前端一致
    gps["speed"] = gps_speed;            // 添加速度字段

    size_t n = serializeJson(doc, json_buffer, sizeof(json_buffer));
    if (n == 0 || n >= sizeof(json_buffer)) {
        return;
    }

    mqtt.publish(MQTT_TOPIC_SENSORS, json_buffer, n);
}
void RadarMotorUploadTask(void* pvParameters) {
    Serial1.begin(115200, SERIAL_8N1, RADAR_RX_PIN, -1);
    gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

    unsigned long lastUpload = 0;
    while (true) {
        while (Serial1.available()) {
            uint8_t b = Serial1.read();
            switch (lidar_state) {
                case WAIT_HEADER_AA: if (b == 0xAA) lidar_state = WAIT_HEADER_55; break;
                case WAIT_HEADER_55: if (b == 0x55) lidar_state = READ_CT; else if (b != 0xAA) lidar_state = WAIT_HEADER_AA; break;
                case READ_CT: packet_ct = b; lidar_state = READ_LSN; break;
                case READ_LSN: packet_lsn = b; payload_expected = 6 + packet_lsn * 2; payload_idx = 0; lidar_state = READ_PAYLOAD; break;
                case READ_PAYLOAD: payload_buf[payload_idx++] = b; if (payload_idx >= payload_expected) { processRadarPacket(); lidar_state = WAIT_HEADER_AA; } break;
            }
        }
        parseGPSNMEA();
        unsigned long now = millis();
        smartAvoid();

        if (WiFi.status() == WL_CONNECTED) {
            // 确保 MQTT 连接成功
            if (!mqtt.connected()) {
                mqtt_reconnect();
            }

        if (mqtt.connected()) {
                mqtt.loop();  // 保活

                // 检查TTS请求超时（防止卡死）
                if (getTTSRequesting() && (millis() - tts_request_start_time > TTS_REQUEST_TIMEOUT_MS)) {
                    Serial.println("[TTS] 请求超时，重置标志");
                    setTTSRequesting(false);
                }

                // 障碍物检测和语音告警（独立控制频率）
                checkObstacleAndAlert();

                // 数据上传 - 每200ms一次，与语音播报频率无关
                if (now - lastUpload >= UPLOAD_INTERVAL_MS) {
                    lastUpload = now;
                    publishSensorData();
                }
            }
        }
        vTaskDelay(15 / portTICK_PERIOD_MS);
    }
}

// ==================== Core 1 导航任务（带路口播报）====================
void NavigationTask(void* pvParameters) {
    Serial.println("[导航] 启动");
    static int last_step_idx = -1;

    while (true) {
        int total = nav_total_steps;
        if (total > 0 && current_step_idx < total && nav_active) {
            // 检测是否进入新路段
            if (current_step_idx != last_step_idx) {
                last_step_idx = current_step_idx;

                // 播报当前路段指引
                String current_instruction = nav_steps[current_step_idx];
                String announcement = "";

                if (current_step_idx == 0) {
                    // 第一步，已包含在planWalkingRoute的播报中，这里不重复
                } else if (current_step_idx >= total - 1) {
                    // 最后一步
                    announcement = "即将到达目的地，" + current_instruction;
                } else {
                    // 中间步骤
                    announcement = "下一个路口，" + current_instruction;
                }

                // 播报路段指引（如果不是第一步）
                if (announcement.length() > 0 && mqtt.connected()) {
                    StaticJsonDocument<512> doc;
                    doc["text"] = announcement;
                    doc["priority"] = PRIO_NORMAL;
                    char buf[512];
                    size_t len = serializeJson(doc, buf, sizeof(buf));
                    mqtt.publish("blindstick/tts/request", buf, len);
                }
            }

            if (current_progress < 100) {
                if (is_blocked || is_ai_talking) {
                    vTaskDelay(200 / portTICK_PERIOD_MS);
                    continue;
                }
                // 每3秒增加5%进度，一步约60秒
                vTaskDelay(3000 / portTICK_PERIOD_MS);
                current_progress += 5;
            } else {
                current_progress = 0;
                current_step_idx++;

                if (current_step_idx >= total) {
                    nav_active = false;
                    nav_total_steps = 1;
                    nav_steps[0] = "导航完成，请说出新目的地";
                    last_step_idx = -1;
                    Serial.println("[导航] 完成");

                    // 播报导航完成
                    if (mqtt.connected()) {
                        StaticJsonDocument<256> doc;
                        doc["text"] = "导航完成，已到达目的地";
                        doc["priority"] = PRIO_NORMAL;
                        char buf[256];
                        size_t len = serializeJson(doc, buf, sizeof(buf));
                        mqtt.publish("blindstick/tts/request", buf, len);
                    }
                }
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
        } else {
            current_step_idx = 0;
            current_progress = 0;
            last_step_idx = -1;
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// ==================== I2S 初始化 ====================
void i2s_init() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_IRAM,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = false,
        .fixed_mclk = 0
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK_PIN,
        .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE,
        .data_in_num = I2S_SD_PIN
    };

    esp_err_t err = i2s_driver_install(I2S_PORT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[I2S] 麦克风初始化失败: %d\n", err);
        return;
    }
    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[I2S] 麦克风引脚设置失败: %d\n", err);
        return;
    }
}

void i2s_out_init() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 16000,
        .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_IRAM,
        .dma_buf_count = 4,
        .dma_buf_len = 256,
        .use_apll = false,
        .tx_desc_auto_clear = true,
        .fixed_mclk = 0
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_PIN,
        .ws_io_num = I2S_WS_OUT_PIN,
        .data_out_num = I2S_DATA_PIN,
        .data_in_num = I2S_PIN_NO_CHANGE
    };

    esp_err_t err = i2s_driver_install(I2S_PORT_OUT, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[I2S] 扬声器初始化失败: %d\n", err);
        return;
    }
    err = i2s_set_pin(I2S_PORT_OUT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[I2S] 扬声器引脚设置失败: %d\n", err);
        return;
    }
}

// 简单的启动提示音
void playLocalStartupTone() {
    const int sample_rate = 16000;
    const int num_samples = sample_rate / 2;
    static int16_t tone_buffer[8000];
    for (int i = 0; i < num_samples; i++) {
        float t = (float)i / sample_rate;
        float sample = sin(2 * PI * 1000 * t) * 8000.0f;
        tone_buffer[i] = (int16_t)sample;
    }
    size_t written = 0;
    i2s_zero_dma_buffer(I2S_PORT_OUT);
    i2s_write(I2S_PORT_OUT, tone_buffer, num_samples * 2, &written, portMAX_DELAY);
    delay(600);
    i2s_zero_dma_buffer(I2S_PORT_OUT);
}

// 雷达避障提示音 - 嘀嘀嘀（短促高频）
void playAlertBeep() {
    const int sample_rate = 16000;
    const int beep_duration = 100;  // 每个嘀声100ms
    const int pause_duration = 50;  // 间隔50ms
    const int freq = 2000;          // 2000Hz高频

    // 生成单个嘀声
    int16_t beep_buffer[1600];  // 100ms @ 16kHz
    for (int i = 0; i < 1600; i++) {
        float t = (float)i / sample_rate;
        float sample = sin(2 * PI * freq * t) * 10000.0f;  // 较大音量
        // 添加淡入淡出减少爆音
        if (i < 100) sample *= (i / 100.0f);
        if (i > 1500) sample *= ((1600 - i) / 100.0f);
        beep_buffer[i] = (int16_t)sample;
    }

    // 播放三次：嘀-嘀-嘀
    for (int j = 0; j < 3; j++) {
        i2s_write(I2S_PORT_OUT, beep_buffer, sizeof(beep_buffer), NULL, portMAX_DELAY);
        delay(beep_duration);
        i2s_zero_dma_buffer(I2S_PORT_OUT);
        delay(pause_duration);
    }
}

/**
 * 流式语音识别任务（开机就开始，边录边传）
 */
void VoiceRecognitionTask(void* pvParameters) {
    Serial.println("[语音识别] 启动");

    // 等待WiFi连接
    int waitCount = 0;
    while (WiFi.status() != WL_CONNECTED && waitCount < 60) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        waitCount++;
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[语音识别] WiFi未连接，退出");
        vTaskDelete(NULL);
        return;
    }

    // 等待开机语音播放完成（最多等30秒）
    int waitStartup = 0;
    while (!startup_announced && waitStartup < 30) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        waitStartup++;
    }

    if (startup_announced) {
        Serial.println("[语音识别] 开机语音已播放，开始监听...");
    } else {
        Serial.println("[语音识别] 开始监听...");
    }

    // 主循环：录音4秒 -> 识别 -> 处理结果
    while (true) {
        if (WiFi.status() != WL_CONNECTED) {
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }

        // 录音并识别（4秒）
        String result = doVoiceRecognition();

        if (result.length() > 0) {
            Serial.printf("[语音识别] 识别: %s\n", result.c_str());
            handleVoiceCommand(result.c_str());
            // 等待TTS播报完成
            vTaskDelay(6000 / portTICK_PERIOD_MS);
        } else {
            // 未识别到语音，短暂等待后继续
            vTaskDelay(500 / portTICK_PERIOD_MS);
        }
    }
}

/**
 * 语音识别主函数（录音4秒，REST API）
 * 返回值：识别到的文本，空字符串表示未识别
 */
String doVoiceRecognition() {
    // 录音4秒 = 128KB (16000Hz * 2字节 * 4秒)
    const int RECORD_SIZE = 16000 * 2 * 4;
    uint8_t* buffer = NULL;

    // 优先使用PSRAM
    if (ESP.getPsramSize() > 0) {
        buffer = (uint8_t*)ps_malloc(RECORD_SIZE);
    } else {
        buffer = (uint8_t*)malloc(RECORD_SIZE);
    }

    if (!buffer) {
        Serial.println("[ASR] 内存分配失败");
        return "";
    }

    // 录音4秒
    size_t totalRead = 0;
    unsigned long startTime = millis();
    while (millis() - startTime < 4000 && totalRead < RECORD_SIZE) {
        size_t bytesRead = 0;
        i2s_read(I2S_PORT, buffer + totalRead, RECORD_SIZE - totalRead, &bytesRead, 50);
        totalRead += bytesRead;
        // 喂看门狗，防止复位
        vTaskDelay(1 / portTICK_PERIOD_MS);
    }

    // 检查录音数据是否有效（避免静音）
    int16_t* samples = (int16_t*)buffer;
    int nonZeroCount = 0;
    for (int i = 0; i < totalRead / 2; i++) {
        if (samples[i] > 100 || samples[i] < -100) nonZeroCount++;
        // 每1000个样本喂一次看门狗
        if (i % 1000 == 0) vTaskDelay(1 / portTICK_PERIOD_MS);
    }

    if (nonZeroCount < 100) {
        // 录音数据几乎是静音，跳过识别
        free(buffer);
        return "";
    }

    // 获取百度Token
    String token = getBaiduToken();
    if (token.length() == 0) {
        free(buffer);
        return "";
    }

    // Base64编码
    String base64Audio = base64Encode(buffer, totalRead);
    free(buffer);

    if (base64Audio.length() == 0) {
        return "";
    }

    // 发送ASR请求
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;
    if (!http.begin(client, "https://vop.baidu.com/server_api")) {
        return "";
    }

    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);

    // 构建JSON请求（使用手动拼接避免内存问题）
    String jsonPayload;
    jsonPayload.reserve(base64Audio.length() + 200);
    jsonPayload = "{\"format\":\"pcm\",\"rate\":16000,\"channel\":1,\"cuid\":\"esp32_blindstick\",\"token\":\"";
    jsonPayload += token;
    jsonPayload += "\",\"dev_pid\":1537,\"speech\":\"";
    jsonPayload += base64Audio;
    jsonPayload += "\",\"len\":";
    jsonPayload += totalRead;
    jsonPayload += "}";

    // 喂看门狗，防止HTTP请求阻塞导致复位
    vTaskDelay(1 / portTICK_PERIOD_MS);

    int httpCode = http.POST(jsonPayload);
    String result = "";

    if (httpCode == 200) {
        String response = http.getString();
        // 喂看门狗
        vTaskDelay(1 / portTICK_PERIOD_MS);

        StaticJsonDocument<1024> respDoc;
        DeserializationError error = deserializeJson(respDoc, response);

        if (!error && respDoc["err_no"] == 0) {
            JsonArray results = respDoc["result"];
            if (results.size() > 0) {
                result = results[0].as<String>();
                // 去除标点符号
                result.replace("。", "");
                result.replace("，", "");
                result.replace("？", "");
                result.replace("！", "");
                result.trim();
            }
        }
    }

    http.end();
    return result;
}

/**
 * 获取百度Access Token（带缓存）
 */
String getBaiduToken() {
    // 检查缓存的token是否有效（提前5分钟过期）
    static String cached_token = "";
    static unsigned long expire_time = 0;

    if (cached_token.length() > 0 && millis() < expire_time - 300000) {
        return cached_token;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10000);

    HTTPClient http;
    String url = "https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials";
    url += "&client_id=" + BAIDU_API_KEY;
    url += "&client_secret=" + BAIDU_SECRET_KEY;

    if (!http.begin(client, url)) {
        return "";
    }

    http.setTimeout(10000);
    // 喂看门狗
    vTaskDelay(1 / portTICK_PERIOD_MS);

    int httpCode = http.GET();

    if (httpCode == 200) {
        String response = http.getString();
        // 喂看门狗
        vTaskDelay(1 / portTICK_PERIOD_MS);

        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, response);

        if (!error && doc.containsKey("access_token")) {
            cached_token = doc["access_token"].as<String>();
            int expiresIn = doc["expires_in"] | 2592000;
            expire_time = millis() + (expiresIn * 1000);
            http.end();
            return cached_token;
        }
    }

    http.end();
    return "";
}

/**
 * Base64编码
 */
String base64Encode(const uint8_t* data, size_t len) {
    static const char base64Chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    String encoded = "";
    uint8_t temp[3];
    size_t i = 0;

    while (i < len) {
        int remain = len - i;
        for (int j = 0; j < 3; j++) {
            temp[j] = (i + j < len) ? data[i + j] : 0;
        }

        encoded += base64Chars[(temp[0] >> 2) & 0x3F];
        encoded += base64Chars[((temp[0] << 4) | (temp[1] >> 4)) & 0x3F];
        encoded += (remain > 1) ? base64Chars[((temp[1] << 2) | (temp[2] >> 6)) & 0x3F] : '=';
        encoded += (remain > 2) ? base64Chars[temp[2] & 0x3F] : '=';

        i += 3;

        // 每编码3000字节喂一次看门狗
        if (i % 3000 == 0) {
            vTaskDelay(1 / portTICK_PERIOD_MS);
        }
    }

    return encoded;
}

// ==================== setup / loop ====================
void setup() {
    Serial.begin(115200);
    randomSeed(millis());  // 初始化随机数种子

    // ===== PSRAM 初始化（必须在内存分配前完成）=====
    if (psramInit()) {
        size_t psram_total = ESP.getPsramSize();
        size_t psram_free = ESP.getFreePsram();
        Serial.printf("[系统] PSRAM: %dKB/%dKB\n", psram_free/1024, psram_total/1024);
    }

    pinMode(MOTOR_IN1, OUTPUT); pinMode(MOTOR_IN2, OUTPUT); pinMode(MOTOR_PWM, OUTPUT);
    motorControl(0);
    pinMode(RADAR_M_CTR_PIN, OUTPUT);
    digitalWrite(RADAR_M_CTR_PIN, HIGH);
    Serial.println("[雷达] 启动");

    nav_total_steps = 1;
    nav_steps[0] = "请说出目的地";
    current_step_idx = 0; current_progress = 0; nav_active = false;

    audioMutex = xSemaphoreCreateMutex();

    // 初始化TTS请求标志互斥锁
    ttsRequestMutex = xSemaphoreCreateMutex();
    if (ttsRequestMutex == NULL) {
        Serial.println("[警告] TTS互斥锁创建失败");
    }

    // 初始化流式TTS
    initStreamingTTS();

    i2s_out_init();
    i2s_init();

    // 播放测试音确认扬声器工作
    playLocalStartupTone();

    // 麦克风硬件诊断测试
    uint8_t test_buf[512];
    size_t bytes_read = 0;
    int test_non_zero = 0;
    for (int test = 0; test < 3; test++) {
        esp_err_t err = i2s_read(I2S_PORT, test_buf, sizeof(test_buf), &bytes_read, 100);
        if (err == ESP_OK && bytes_read > 0) {
            int16_t* samples = (int16_t*)test_buf;
            for (int i = 0; i < bytes_read / 2; i++) {
                if (samples[i] != 0 && samples[i] != -1) test_non_zero++;
            }
        }
        delay(50);
    }
    if (test_non_zero == 0) {
        Serial.println("[警告] 麦克风无数据");
    }

    // 连接WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long wifi_start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - wifi_start > 30000) {
            Serial.println("[WiFi] 连接超时");
            break;
        }
        delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("[WiFi] 已连接: %s\n", WiFi.localIP().toString().c_str());

        // 【关键】先配置TLS客户端，再初始化MQTT
        espClient.setInsecure();
        espClient.setHandshakeTimeout(8);

        // 初始化 MQTT
        mqtt.setServer(MQTT_BROKER, MQTT_PORT);
        mqtt.setCallback(mqtt_callback);

        // 连接MQTT（非阻塞重试）
        mqtt_reconnect();

        // 注意：开机语音已移到 mqtt_reconnect 的 on_connect 回调中发送
        // 确保 MQTT 连接成功后才发送，避免发送失败
    } else {
        Serial.println("[WiFi] 离线模式");
    }

    Serial.println("[系统] 等待语音输入目的地...");

    xTaskCreatePinnedToCore(RadarMotorUploadTask, "RadarTask", 8192, NULL, 3, &RadarTaskHandle, 0);
    xTaskCreatePinnedToCore(NavigationTask, "NavTask", 2048, NULL, 1, &NavTaskHandle, 1);
    xTaskCreatePinnedToCore(VoiceRecognitionTask, "VoiceRecTask", 8192, NULL, 2, &VoiceTaskHandle, 1);

    delay(300);
}

void loop() {
    vTaskDelete(NULL);
}

// ==================== 工具函数 ====================

/**
 * 从文本中提取目的地
 */
String extractDestination(const char* text) {
    String input = String(text);
    String destination = "";
    int triggerIndex = -1;

    // 查找触发词
    for (int i = 0; i < NAV_TRIGGER_COUNT; i++) {
        int idx = input.indexOf(NAV_TRIGGERS[i]);
        if (idx != -1 && (triggerIndex == -1 || idx < triggerIndex)) {
            triggerIndex = idx;
        }
    }

    if (triggerIndex == -1) {
        return destination; // 空字符串
    }

    // 提取触发词后的内容
    for (int i = 0; i < NAV_TRIGGER_COUNT; i++) {
        int idx = input.indexOf(NAV_TRIGGERS[i]);
        if (idx == triggerIndex) {
            int startPos = idx + strlen(NAV_TRIGGERS[i]);
            destination = input.substring(startPos);
            break;
        }
    }

    destination.trim();

    // 过滤非目的地词汇
    for (int i = 0; i < FILTER_WORD_COUNT; i++) {
        destination.replace(FILTER_WORDS[i], "");
    }

    destination.trim();

    // 去除标点
    destination.replace(",", "");
    destination.replace("。", "");
    destination.replace("，", "");
    destination.replace("！", "");
    destination.replace("？", "");
    destination.trim();

    return destination;
}

/**
 * 搜索最近的目的地（使用常住地限制搜索范围）
 */
bool searchNearestDestination(const char* keyword, float& outLat, float& outLng, String& outName, float& outDistance) {
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;
    String url = "https://api.map.baidu.com/place/v2/search?query=" + urlEncode(keyword) +
                 "&region=" + urlEncode(home_city.c_str()) +
                 "&output=json&ak=e9R2xrzLSwLzjMH5fdqHz4dLB0gXwIZW&page_size=5";

    if (!http.begin(client, url)) return false;
    http.setTimeout(15000);
    http.setReuse(false);
    int httpCode = http.GET();

    if (httpCode != 200) {
        http.end();
        return false;
    }

    String response = http.getString();
    StaticJsonDocument<2048> doc;
    DeserializationError error = deserializeJson(doc, response);

    if (error || doc["status"] != 0) {
        http.end();
        return false;
    }

    JsonArray results = doc["results"];
    if (results.size() == 0) {
        http.end();
        return false;
    }

    // 计算当前位置 - 使用实时GPS数据，不再硬编码
    float currentLat = gps_lat;
    float currentLng = gps_lng;

    // 检查GPS是否有效
    if (currentLat < 1.0 || currentLng < 1.0) {
        Serial.println("[导航] GPS未定位，无法搜索目的地");
        // 播报GPS未定位提示
        StaticJsonDocument<256> doc;
        doc["text"] = "GPS未定位，请等待卫星信号";
        doc["priority"] = PRIO_NORMAL;
        char buf[256];
        size_t len = serializeJson(doc, buf, sizeof(buf));
        mqtt.publish("blindstick/tts/request", buf, len);
        return false;
    }

    Serial.printf("[导航] 当前位置: %.6f, %.6f\n", currentLat, currentLng);

    // 找到最近的地点
    float minDistance = 999999999;
    int nearestIdx = 0;

    for (int i = 0; i < results.size(); i++) {
        JsonObject place = results[i];
        if (!place.containsKey("location")) continue;

        float lat = place["location"]["lat"];
        float lng = place["location"]["lng"];

        float distance = calcDistance(currentLat, currentLng, lat, lng);
        if (distance < minDistance) {
            minDistance = distance;
            nearestIdx = i;
        }
    }

    JsonObject nearest = results[nearestIdx];
    outLat = nearest["location"]["lat"];
    outLng = nearest["location"]["lng"];
    outName = nearest["name"].as<String>();
    outDistance = minDistance;

    http.end();
    return true;
}

/**
 * 规划步行路线
 */
bool planWalkingRoute(float destLat, float destLng, String& destName) {
    if (WiFi.status() != WL_CONNECTED) return false;

    // 使用实时GPS数据，不再硬编码
    float originLat = gps_lat;
    float originLng = gps_lng;

    // 检查GPS是否有效
    if (originLat < 1.0 || originLng < 1.0) {
        Serial.println("[导航] GPS未定位，无法规划路线");
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;
    String url = "https://api.map.baidu.com/directionlite/v1/walking?origin=" +
                 String(originLat, 6) + "," + String(originLng, 6) +
                 "&destination=" + String(destLat, 6) + "," + String(destLng, 6) +
                 "&ak=e9R2xrzLSwLzjMH5fdqHz4dLB0gXwIZW";

    if (!http.begin(client, url)) return false;
    http.setTimeout(15000);
    http.setReuse(false);
    int httpCode = http.GET();

    if (httpCode != 200) {
        http.end();
        return false;
    }

    String response = http.getString();
    StaticJsonDocument<4096> doc;
    DeserializationError error = deserializeJson(doc, response);

    if (error || doc["status"] != 0) {
        http.end();
        return false;
    }

    JsonObject route = doc["result"]["routes"][0];
    int distance = route["distance"];
    int duration = route["duration"];
    JsonArray steps = route["steps"];

    // 保存导航信息
    nav_total_steps = min((int)steps.size(), 10);
    for (int i = 0; i < nav_total_steps; i++) {
        String instruction = steps[i]["instruction"];
        // 去除HTML标签
        instruction.replace("<b>", "");
        instruction.replace("</b>", "");
        instruction.replace("<font color='red'>", "");
        instruction.replace("</font>", "");
        nav_steps[i] = instruction;
    }

    nav_active = true;
    current_step_idx = 0;
    current_progress = 0;

    // 播报导航开始 - 使用流式TTS
    char navText[256];
    snprintf(navText, sizeof(navText), "开始导航到%s，全程%d米，预计%d分钟，%s",
             destName.c_str(), distance, duration / 60, nav_steps[0].c_str());
    // 通过MQTT发送给代理服务器进行流式TTS
    StaticJsonDocument<512> ttsDoc;
    ttsDoc["text"] = navText;
    ttsDoc["priority"] = PRIO_NORMAL;
    char buf[512];
    size_t len = serializeJson(ttsDoc, buf, sizeof(buf));
    mqtt.publish("blindstick/tts/request", buf, len);

    http.end();
    return true;
}

/**
 * 处理语音识别结果
 * 流程：
 * 1. 提取目的地（支持触发词+目的地或直接说目的地）
 * 2. 使用GPS当前位置搜索最近的目的地
 * 3. 如果超过10公里，播报"目的地距离超过10公里，请再说一次"并继续监听
 * 4. 如果在10公里内，开始导航并播报导航信息
 */
void handleVoiceCommand(const char* text) {
    Serial.printf("[语音识别] 识别结果: %s\n", text);

    // 提取目的地
    String destination = extractDestination(text);

    // 如果没有触发词，尝试直接使用识别文本作为目的地
    if (destination.length() < 2) {
        Serial.println("[语音识别] 无触发词，尝试直接搜索...");
        destination = String(text);
        // 去除标点和常见语气词
        destination.replace("。", "");
        destination.replace("，", "");
        destination.replace("！", "");
        destination.replace("？", "");
        destination.replace("啊", "");
        destination.replace("吧", "");
        destination.replace("呢", "");
        destination.replace("吗", "");
        destination.replace("哦", "");
        destination.trim();
    }

    if (destination.length() < 2) {
        Serial.println("[语音识别] 未提取到有效目的地");
        return;
    }

    Serial.printf("[语音识别] 目的地: %s\n", destination.c_str());

    // 搜索最近的目的地（使用GPS当前位置）
    float destLat, destLng, distance;
    String destName;

    if (!searchNearestDestination(destination.c_str(), destLat, destLng, destName, distance)) {
        // 发送失败提示 - 使用流式TTS
        StaticJsonDocument<256> doc;
        doc["text"] = "抱歉，没有找到该地点，请重新说出目的地";
        doc["priority"] = PRIO_NORMAL;
        char buf[256];
        size_t len = serializeJson(doc, buf, sizeof(buf));
        mqtt.publish("blindstick/tts/request", buf, len);
        return;
    }

    // 检查距离是否太远（超过10公里）
    if (distance > MAX_NAVIGATION_DISTANCE) {
        char msg[128];
        snprintf(msg, sizeof(msg), "目的地距离超过10公里，请再说一次");
        StaticJsonDocument<256> doc;
        doc["text"] = msg;
        doc["priority"] = PRIO_NORMAL;
        char buf[256];
        size_t len = serializeJson(doc, buf, sizeof(buf));
        mqtt.publish("blindstick/tts/request", buf, len);
        // 不设置nav_active，继续监听新的语音输入
        Serial.println("[导航] 目的地超过10公里，继续监听...");
        return;
    }

    // 在10公里内，开始导航
    Serial.printf("[导航] 找到目的地: %s, 距离: %.1f米\n", destName.c_str(), distance);

    // 规划路线
    if (planWalkingRoute(destLat, destLng, destName)) {
        Serial.printf("[导航] 已开始导航到: %s\n", destName.c_str());
    } else {
        StaticJsonDocument<256> doc;
        doc["text"] = "路线规划失败，请重试";
        doc["priority"] = PRIO_NORMAL;
        char buf[256];
        size_t len = serializeJson(doc, buf, sizeof(buf));
        mqtt.publish("blindstick/tts/request", buf, len);
    }
}


// ==================== TTS URL处理（下载并播放）====================
void handleTTSUrl(const char* payload, int length) {
    // 解析JSON获取URL和文本
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, payload, length);

    if (error) {
        Serial.printf("[TTS-URL] JSON解析失败: %s\n", error.c_str());
        return;
    }

    const char* url = doc["url"];
    const char* text = doc["text"];
    if (!url || strlen(url) == 0) {
        Serial.println("[TTS-URL] URL为空");
        return;
    }

    // 去重：基于文本内容 + 3秒时间窗口
    static String lastText = "";
    static unsigned long lastPlayTime = 0;
    String currentText = text ? String(text) : "";
    unsigned long now = millis();

    if (currentText == lastText && (now - lastPlayTime) < 3000) {
        Serial.println("[TTS-URL] 3秒内重复文本，跳过播放");
        return;
    }
    lastText = currentText;
    lastPlayTime = now;

    Serial.printf("[TTS-URL] 收到URL，开始下载...\n");

    // 暂停语音识别（释放网络带宽）
    if (VoiceTaskHandle != NULL) {
        vTaskSuspend(VoiceTaskHandle);
    }

    // HTTP下载音频 - 优化超时设置
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(20000);  // 增加到20秒，给大文件足够时间

    HTTPClient http;
    if (!http.begin(client, url)) {
        Serial.println("[TTS-URL] HTTP初始化失败");
        setTTSRequesting(false);  // 重置标志
        if (VoiceTaskHandle != NULL) vTaskResume(VoiceTaskHandle);
        return;
    }

    http.setTimeout(25000);  // 增加到25秒
    int httpCode = http.GET();

    if (httpCode != 200) {
        Serial.printf("[TTS-URL] 下载失败: %d\n", httpCode);
        http.end();
        if (VoiceTaskHandle != NULL) vTaskResume(VoiceTaskHandle);
        return;
    }

    int len = http.getSize();
    if (len <= 0 || len > 200000) {
        Serial.printf("[TTS-URL] 音频大小无效: %d\n", len);
        http.end();
        if (VoiceTaskHandle != NULL) vTaskResume(VoiceTaskHandle);
        return;
    }

    Serial.printf("[TTS-URL] 音频%d字节，下载中...\n", len);

    // 分配内存（优先使用PSRAM）
    uint8_t* audioBuffer = NULL;
    if (ESP.getPsramSize() > 0) {
        audioBuffer = (uint8_t*)ps_malloc(len);
    } else {
        audioBuffer = (uint8_t*)malloc(len);
    }

    if (!audioBuffer) {
        Serial.println("[TTS-URL] 内存分配失败");
        http.end();
        if (VoiceTaskHandle != NULL) vTaskResume(VoiceTaskHandle);
        return;
    }

    // 读取数据 - 使用更大的缓冲区
    WiFiClient* stream = http.getStreamPtr();
    int totalRead = 0;
    int bufferSize = 8192;  // 增大到8KB缓冲区，提高下载效率
    unsigned long downloadStart = millis();
    unsigned long lastDataTime = millis();  // 上次收到数据的时间
    const unsigned long DOWNLOAD_TIMEOUT_MS = 30000;  // 增加到30秒总超时
    const unsigned long DATA_TIMEOUT_MS = 10000;      // 10秒无数据超时

    Serial.printf("[TTS-URL] 开始下载，大小:%d字节\n", len);

    while (totalRead < len) {
        // 检查连接状态
        if (!stream->connected()) {
            Serial.println("[TTS-URL] 连接断开");
            break;
        }

        int available = stream->available();
        if (available > 0) {
            int toRead = min(available, min(bufferSize, len - totalRead));
            int r = stream->readBytes(audioBuffer + totalRead, toRead);
            if (r > 0) {
                totalRead += r;
                lastDataTime = millis();  // 更新最后数据时间

                // 每下载10KB打印进度
                if (totalRead % 10240 == 0 || totalRead == len) {
                    Serial.printf("[TTS-URL] 下载进度: %d/%d (%.1f%%)\n",
                        totalRead, len, (float)totalRead / len * 100);
                }
            }
        } else {
            // 没有数据可用，短暂等待
            delay(5);
        }

        // 总超时检查
        if (millis() - downloadStart > DOWNLOAD_TIMEOUT_MS) {
            Serial.printf("[TTS-URL] 下载总超时(>%ds)，已下载:%d/%d\n",
                DOWNLOAD_TIMEOUT_MS/1000, totalRead, len);
            break;
        }

        // 无数据超时检查（防止卡在半途中）
        if (millis() - lastDataTime > DATA_TIMEOUT_MS) {
            Serial.printf("[TTS-URL] 数据接收超时(>%ds无数据)，已下载:%d/%d\n",
                DATA_TIMEOUT_MS/1000, totalRead, len);
            break;
        }

        if (totalRead >= len) break;

        // 喂狗，防止看门狗复位
        yield();
    }

    http.end();

    if (totalRead != len) {
        float percent = (float)totalRead / len * 100;
        Serial.printf("[TTS-URL] 下载不完整: %d/%d (%.1f%%)\n", totalRead, len, percent);

        // 如果下载了超过80%，尝试播放已下载的部分
        if (totalRead > len * 0.8 && totalRead > 10240) {
            Serial.println("[TTS-URL] 下载超过80%，尝试播放...");

            // 跳过WAV头并播放
            int offset = 0;
            if (totalRead > 44 && audioBuffer[0] == 'R' && audioBuffer[1] == 'I') {
                offset = 44;
            }
            playPcmData(audioBuffer + offset, totalRead - offset);

            free(audioBuffer);
            Serial.println("[TTS-URL] 播放完成(部分下载)");

            // 重置TTS请求标志
            setTTSRequesting(false);

            // 恢复语音识别
            if (VoiceTaskHandle != NULL) {
                eTaskState taskState = eTaskGetState(VoiceTaskHandle);
                if (taskState == eSuspended) {
                    vTaskResume(VoiceTaskHandle);
                }
            }
            return;
        }

        // 下载太少，放弃并释放内存
        free(audioBuffer);
        // 重置TTS请求标志
        setTTSRequesting(false);
        if (VoiceTaskHandle != NULL) vTaskResume(VoiceTaskHandle);
        return;
    }

    Serial.printf("[TTS-URL] 下载完成，播放中...\n");

    // 跳过WAV头并播放
    int offset = 0;
    if (len > 44 && audioBuffer[0] == 'R' && audioBuffer[1] == 'I') {
        offset = 44;
    }
    playPcmData(audioBuffer + offset, len - offset);

    free(audioBuffer);
    Serial.println("[TTS-URL] 播放完成");

    // 重置TTS请求标志（播放完成，可以发送下一个请求）
    setTTSRequesting(false);

    // 恢复语音识别 - 确保正确恢复
    if (VoiceTaskHandle != NULL) {
        eTaskState taskState = eTaskGetState(VoiceTaskHandle);
        if (taskState == eSuspended) {
            vTaskResume(VoiceTaskHandle);
        }
    }
}

// ==================== 流式TTS实现（新版简化逻辑）====================

void initStreamingTTS() {
    if (ESP.getPsramSize() > 0) {
        stream_buffer = (uint8_t*)ps_malloc(STREAM_BUF_SIZE);
    } else {
        stream_buffer = (uint8_t*)malloc(STREAM_BUF_SIZE);
    }
    if (stream_buffer) {
        Serial.printf("[流式TTS] 缓冲区分配成功: %d字节\n", STREAM_BUF_SIZE);
    } else {
        Serial.println("[流式TTS] 缓冲区分配失败！");
    }
    stream_playing = false;
    stream_priority = 0;
    stream_buf_used = 0;
}

void playPcmData(uint8_t* data, int len) {
    if (!data || len < 2) return;
    int16_t* samples = (int16_t*)data;
    int num_samples = len / 2;
    for (int i = 0; i < num_samples; i += 256) {
        int16_t temp_buffer[256];
        int chunk_samples = min(256, num_samples - i);
        for (int j = 0; j < chunk_samples; j++) {
            int32_t sample = (int32_t)(samples[i + j] * VOLUME_GAIN);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            temp_buffer[j] = (int16_t)sample;
        }
        size_t bytes_written = 0;
        i2s_write(I2S_PORT_OUT, temp_buffer, chunk_samples * 2, &bytes_written, portMAX_DELAY);
    }
    // 等待播放完成
    int audio_duration_ms = (len / 32000.0) * 1000;
    delay(audio_duration_ms + 100);
}

void stopCurrentPlayback() {
    i2s_zero_dma_buffer(I2S_PORT_OUT);
    stream_playing = false;
    stream_buf_used = 0;
    Serial.println("[流式TTS] 停止当前播放");
}

void handleStreamControl(const char* payload, int length) {
    StaticJsonDocument<256> doc;
    DeserializationError err = deserializeJson(doc, payload, length);
    if (err) {
        Serial.println("[流式TTS] 控制消息解析失败");
        return;
    }

    const char* type = doc["type"] | "unknown";
    int new_priority = doc["priority"] | 0;
    unsigned long session_id = doc["session_id"] | 0;

    if (strcmp(type, "stream_start") == 0) {
        Serial.printf("[流式TTS] 开始新会话 priority=%s session=%lu\n",
                     getPrioName(new_priority), session_id);

        if (stream_playing) {
            if (new_priority >= stream_priority) {
                Serial.printf("[流式TTS] 打断当前%s播放\n", getPrioName(stream_priority));
                stopCurrentPlayback();
            } else {
                Serial.printf("[流式TTS] 忽略低优先级%s\n", getPrioName(new_priority));
                return;
            }
        }

        stream_playing = true;
        stream_priority = new_priority;
        stream_session_id = session_id;
        stream_buf_used = 0;

        if (VoiceTaskHandle != NULL) {
            vTaskSuspend(VoiceTaskHandle);
        }

    } else if (strcmp(type, "stream_end") == 0) {
        int segments = doc["segments"] | 0;
        Serial.printf("[流式TTS] 会话结束，共%d段\n", segments);

        if (stream_buf_used > 0 && stream_playing) {
            playPcmData(stream_buffer, stream_buf_used);
        }

        if (VoiceTaskHandle != NULL && stream_priority < PRIO_HIGH) {
            eTaskState taskState = eTaskGetState(VoiceTaskHandle);
            if (taskState == eSuspended) {
                vTaskResume(VoiceTaskHandle);
                Serial.println("[流式TTS] 语音识别已恢复");
            }
        }

        stream_playing = false;
        stream_buf_used = 0;

    } else if (strcmp(type, "interrupt") == 0) {
        Serial.printf("[流式TTS] 收到打断信号\n");
        stopCurrentPlayback();
        if (VoiceTaskHandle != NULL) {
            eTaskState taskState = eTaskGetState(VoiceTaskHandle);
            if (taskState == eSuspended) {
                vTaskResume(VoiceTaskHandle);
                Serial.println("[流式TTS] 语音识别已恢复");
            }
        }
    }
}

void handleStreamAudio(const char* topic, byte* payload, unsigned int length) {
    if (!stream_playing) return;

    int segment_idx = 0;
    const char* last_slash = strrchr(topic, '/');
    if (last_slash) {
        segment_idx = atoi(last_slash + 1);
    }

    // 高优先级立即播放，不缓冲
    if (stream_priority == PRIO_HIGH) {
        Serial.printf("[流式TTS] 立即播放第%d段: %d字节\n", segment_idx, length);
        playPcmData(payload, length);
        return;
    }

    // 普通优先级使用缓冲
    if (stream_buffer && stream_buf_used + length <= STREAM_BUF_SIZE) {
        memcpy(stream_buffer + stream_buf_used, payload, length);
        stream_buf_used += length;

        // 缓冲区半满时播放一半
        if (stream_buf_used >= STREAM_BUF_SIZE / 2) {
            int play_size = stream_buf_used / 2;
            playPcmData(stream_buffer, play_size);
            memmove(stream_buffer, stream_buffer + play_size, stream_buf_used - play_size);
            stream_buf_used -= play_size;
        }
    }
}