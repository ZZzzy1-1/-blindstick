#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <HardwareSerial.h>
#include <PubSubClient.h>
#include <driver/i2s.h>
#include <WebSocketsClient.h>  // 流式语音识别需要
#include <freertos/FreeRTOS.h>  // FreeRTOS任务控制
#include <freertos/task.h>
#include <esp_heap_caps.h>      // PSRAM内存分配

// ==================== 网络参数 ====================
const char* WIFI_SSID     = "ZZY";
const char* WIFI_PASSWORD = "zzy060630";

// ==================== MQTT 参数（EMQX Cloud） ====================
// 如果8883端口(TLS)一直连不上，可以尝试以下端口：
// 1883 - MQTT over TCP（明文，不需要证书，但不是所有EMQX实例都开放）
// 8883 - MQTT over TLS（需要证书处理）
const char* MQTT_BROKER   = "u72a7838.ala.asia-southeast1.emqxsl.com";
const int   MQTT_PORT     = 8883;  // 如果8883不行，尝试改成1883
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

// ==================== 高德地图参数 ====================
const String AMAP_KEY     = "90a13b148658cc0a0525f959202a4063";
const String NAV_DEST_NAME    = "湖北师范大学教育大楼";
const String NAV_ORIGIN       = "115.063977,30.229320";

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
// WS  → GPIO5  (LRCK)
// SCK → GPIO2  (BCLK)
// SD  → GPIO8  (MIC_IN)
// GND → GND
// L/R → GND (接地=左声道)
#define I2S_WS_PIN      5   // LRCK
#define I2S_SCK_PIN     2   // BCLK
#define I2S_SD_PIN      8   // MIC_IN
#define I2S_PORT        I2S_NUM_0

// ==================== I2S扬声器引脚 (MAX98357) ====================
#define I2S_BCK_PIN     47  // SPK_BCLK
#define I2S_WS_OUT_PIN  41  // SPK_LRCK
#define I2S_DATA_PIN    21  // SPK_OUT
#define I2S_PORT_OUT    I2S_NUM_1

#define VOLUME_GAIN     0.6

// ==================== 音频采样参数 ====================
#define SAMPLE_RATE     16000  // 16kHz 采样率

// ==================== 函数声明 ====================
String urlEncode(const char* str);
float calcDistance(float lat1, float lng1, float lat2, float lng2);

// 流式TTS相关函数
void initStreamingTTS();
void handleStreamControl(const char* payload, int length);
void handleStreamAudio(const char* topic, byte* payload, unsigned int length);
void playPcmData(uint8_t* data, int len);
void stopCurrentPlayback();
const char* getPrioName(int p);
void announceObstacle(float distance, const char* direction);

// 流式语音识别相关
void VoiceRecognitionTask(void* pvParameters);
void startStreamingASR();
void stopStreamingASR();
void sendAudioChunk();
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length);
String doRESTASR();
void handleVoiceCommand(const char* text);
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length);
String doRESTASR();  // REST API备选方案
String base64Encode(const uint8_t* data, size_t len);  // Base64编码

// 测试模式相关函数
void testRecordAndPlaybackTask(void* pvParameters);
void handleTestAudio(const char* topic, byte* payload, unsigned int length);

// 流式语音识别全局变量
WebSocketsClient webSocket;
volatile bool asrConnected = false;
volatile bool asrFinished = false;
String asrResult = "";

#define ASR_CHUNK_SIZE 5120  // 160ms PCM数据 @ 16kHz 16bit

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

int last_motor_pwm = 0;
String last_motor_dir = "stop";

float gps_lat = 0.0;
float gps_lng = 0.0;
float gps_speed = 0.0;
int   gps_heading = 0;
int   gps_satellites = 0;

volatile bool amap_fused = false;

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
#define NUM_DIR     5
#define SMOOTH_A    0.50f

volatile float dir_raw[NUM_DIR] = {400.0f, 400.0f, 400.0f, 400.0f, 400.0f};
volatile float dir_smt[NUM_DIR] = {400.0f, 400.0f, 400.0f, 400.0f, 400.0f};

// ==================== 电机控制 ====================
void motorControl(int steerPower) {
    if (is_ai_talking) {
        digitalWrite(MOTOR_IN1, LOW); digitalWrite(MOTOR_IN2, LOW);
        analogWrite(MOTOR_PWM, 0);
        last_motor_dir = "stop"; last_motor_pwm = 0; return;
    }
    int safe = constrain(steerPower, -STEER_MAX_PWM, STEER_MAX_PWM);
    last_motor_pwm = safe;
    if (safe > 30) {
        digitalWrite(MOTOR_IN1, HIGH); digitalWrite(MOTOR_IN2, LOW);
        analogWrite(MOTOR_PWM, safe); last_motor_dir = "right";
    } else if (safe < -30) {
        digitalWrite(MOTOR_IN1, LOW); digitalWrite(MOTOR_IN2, HIGH);
        analogWrite(MOTOR_PWM, abs(safe)); last_motor_dir = "left";
    } else {
        digitalWrite(MOTOR_IN1, LOW); digitalWrite(MOTOR_IN2, LOW);
        analogWrite(MOTOR_PWM, 0); last_motor_dir = "stop";
    }
}

// ==================== 避障决策 ====================
void smartAvoidDecision() {
    float f  = dir_smt[0];
    float fR = dir_smt[1];
    float fL = dir_smt[2];
    float R  = dir_smt[3];
    float L  = dir_smt[4];
    static unsigned long turnStartMs = 0;
    static bool was_turning = false;
    unsigned long now = millis();

    bool front_blocked = (f < 150.0f) || (fL < 150.0f) || (fR < 150.0f);
    bool side_alert    = (L < 70.0f)  || (R < 70.0f);
    is_blocked = front_blocked || side_alert;

    if (front_blocked) {
        turnStartMs = now; was_turning = true;
        if (fL > fR)  motorControl( STEER_MAX_PWM);
        else          motorControl(-STEER_MAX_PWM);
        return;
    }
    if (was_turning) {
        bool escape_clear;
        if (fL > fR)  escape_clear = (fL > 250.0f && f > 200.0f);
        else          escape_clear = (fR > 250.0f && f > 200.0f);
        bool timeout = (now - turnStartMs) > 2500;
        if (escape_clear || timeout) {
            was_turning = false;
        } else {
            if (fL > fR)  motorControl( STEER_MAX_PWM);
            else          motorControl(-STEER_MAX_PWM);
            return;
        }
    }
    if (side_alert) {
        float leftPull  = (70.0f - L) * 3.0f;
        float rightPull = (70.0f - R) * 3.0f;
        motorControl((int)(leftPull - rightPull));
        return;
    }
    if (R < 200.0f) {
        float err = R - 90.0f;
        err = constrain(err, -50.0f, 60.0f);
        motorControl((int)(err * 0.4f));
        return;
    }
    motorControl(0);
}

// ==================== 雷达处理 ====================
static inline int point_to_dir(float ang) {
    // 如果雷达装反了（旋转180°），进行角度修正
    float corrected_ang = ang + 180.0f;
    if (corrected_ang >= 360.0f) corrected_ang -= 360.0f;
    
    // 用修正后的角度进行方向映射
    if (corrected_ang >= 340.0f || corrected_ang <= 20.0f) {
        return 0;  // 正前方
    } else if (corrected_ang > 20.0f && corrected_ang <= 60.0f) {
        return 2;  // 左前方
    } else if (corrected_ang > 60.0f && corrected_ang <= 120.0f) {
        return 4;  // 左侧
    } else if (corrected_ang >= 240.0f && corrected_ang <= 300.0f) {
        return 3;  // 右侧
    } else if (corrected_ang > 300.0f && corrected_ang < 340.0f) {
        return 1;  // 右前方
    } else {
        return -1; // 无效方向（后方）
    }
}

void processRadarPacket() {
    uint16_t fsa = payload_buf[0] | (payload_buf[1] << 8);
    uint16_t lsa = payload_buf[2] | (payload_buf[3] << 8);
    float angleFSA = (fsa >> 1) / 64.0f;
    float angleLSA = (lsa >> 1) / 64.0f;
    float diffAngle = angleLSA - angleFSA;
    if (diffAngle < 0) diffAngle += 360.0f;
    for (int s = 0; s < NUM_DIR; s++) dir_raw[s] = 400.0f;

    for (int i = 0; i < packet_lsn; i++) {
        uint16_t si = payload_buf[6 + i * 2] | (payload_buf[6 + i * 2 + 1] << 8);
        float mm = si / 4.0f;
        if (mm > 30.0f && mm < 8000.0f) {
            float cm  = mm / 10.0f;
            float ang = angleFSA + (packet_lsn > 1 ? (diffAngle / (packet_lsn - 1)) * i : 0);
            if (ang >= 360.0f) ang -= 360.0f;
            int d = point_to_dir(ang);
            if (d >= 0 && cm < dir_raw[d]) dir_raw[d] = cm;
        }
    }
    for (int s = 0; s < NUM_DIR; s++) {
        dir_smt[s] = SMOOTH_A * dir_raw[s] + (1.0f - SMOOTH_A) * dir_smt[s];
    }
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
                idx = 0;
            }
        }
    }
}

bool fetchAmapRoute() {
    if (amap_fused) {
        nav_total_steps = 3;
        nav_steps[0] = "前方直行，跟随电机牵引和雷达避障";
        nav_steps[1] = "前方进入开阔路段，保持安全直行";
        nav_steps[2] = "已接近湖北师范大学目的地附近";
        return false;
    }
    static volatile bool is_fetching = false;
    if (is_fetching) return false;
    is_fetching = true;
    if (WiFi.status() != WL_CONNECTED) { is_fetching = false; return false; }
    String originCoord = NAV_ORIGIN;
    if (gps_lat > 1.0 && gps_lng > 1.0) originCoord = String(gps_lng, 6) + "," + String(gps_lat, 6);
    HTTPClient http;
    String destCoord = "";
    String geoUrl = "https://restapi.amap.com/v3/geocode/geo?address=" + NAV_DEST_NAME + "&city=黄石&key=" + AMAP_KEY;
    http.begin(geoUrl);
    int geoCode = http.GET();
    if (geoCode == 200) {
        StaticJsonDocument<1024> geoDoc;
        String resp = http.getString();
        if (resp.indexOf("402") != -1 || resp.indexOf("INVALID_USER_KEY") != -1) {
            Serial.println("\n[熔断] 高德限制，切换本地看护模式");
            amap_fused = true; http.end(); is_fetching = false; return false;
        }
        if (deserializeJson(geoDoc, resp) == DeserializationError::Ok) {
            if (geoDoc["status"] == "1" && geoDoc["geocodes"].size() > 0)
                destCoord = geoDoc["geocodes"][0]["location"].as<String>();
        }
    }
    http.end();
    if (destCoord.isEmpty()) destCoord = "115.040000,30.220000";
    String routeUrl = "https://restapi.amap.com/v3/direction/walking?origin=" + originCoord + "&destination=" + destCoord + "&key=" + AMAP_KEY;
    http.begin(routeUrl);
    int code = http.GET();
    if (code == 200) {
        String responseStr = http.getString();
        if (responseStr.indexOf("402") != -1 || responseStr.indexOf("USERKEY_PLAT_NOMATCH") != -1 || responseStr.indexOf("INVALID_USER_KEY") != -1) {
            Serial.println("\n[熔断] 高德402，本地看护模式");
            amap_fused = true;
            nav_total_steps = 3;
            nav_steps[0] = "前方直行，跟随电机牵引和雷达避障";
            nav_steps[1] = "前方进入开阔路段，保持安全直行";
            nav_steps[2] = "已接近湖北师范大学目的地附近";
            current_step_idx = 0; current_progress = 0;
            http.end(); is_fetching = false; return false;
        }
        DynamicJsonDocument doc(4096);
        if (deserializeJson(doc, responseStr) == DeserializationError::Ok && doc["status"] == "1") {
            JsonArray steps = doc["route"]["paths"][0]["steps"].as<JsonArray>();
            nav_total_steps = 0;
            for (JsonObject step : steps) {
                nav_steps[nav_total_steps] = step["instruction"].as<String>();
                nav_total_steps++; if (nav_total_steps >= 10) break;
            }
            current_step_idx = 0; current_progress = 0;
            http.end(); is_fetching = false; return true;
        }
    }
    http.end();
    nav_total_steps = 2;
    nav_steps[0] = "前方直行，请注意雷达避障";
    nav_steps[1] = "继续朝着目的地前行";
    current_step_idx = 0; current_progress = 0;
    is_fetching = false; return false;
}

// ==================== MQTT 重连 ====================
void mqtt_reconnect() {
    static bool network_tested = false;

    // 先测试网络连通性
    if (!network_tested) {
        Serial.println("[网络测试] 测试外网连接...");
        WiFiClient testClient;
        testClient.setTimeout(3000);
        if (testClient.connect("baidu.com", 80)) {
            Serial.println("[网络测试] ✓ 可以访问百度，外网正常");
            testClient.stop();
        } else {
            Serial.println("[网络测试] ✗ 无法访问外网，检查WiFi网络");
        }
        network_tested = true;
    }

    // 配置MQTT客户端参数（TLS已在setup中配置）
    mqtt.setSocketTimeout(10);  // 增加socket超时到10秒
    mqtt.setKeepAlive(60);      // 增加keepalive到60秒
    mqtt.setBufferSize(131072);  // 增加到128KB，支持更大音频+MQTT开销
    // 注意：callback已在setup中设置，这里不需要重复设置

    // 【关键】打印配置信息
    Serial.printf("[MQTT] 配置: broker=%s:%d, SocketTimeout=10s, KeepAlive=60s\n",
                  MQTT_BROKER, MQTT_PORT);
    Serial.printf("[MQTT] 缓冲区大小: %d 字节\n", 131072);

    int retryCount = 0;
    while (!mqtt.connected()) {
        Serial.printf("[MQTT] 尝试连接 #%d broker=%s:%d...\n", retryCount + 1, MQTT_BROKER, MQTT_PORT);
        Serial.printf("[MQTT] ClientID=%s, User=%s\n", MQTT_CLIENT_ID, MQTT_USER);

        // 【重要】每次重试前重新配置TLS（测试显示第一次可以成功，重试需要重新设置）
        espClient.setInsecure();
        espClient.setHandshakeTimeout(12);  // 12秒握手超时

        // 确保WiFi连接状态正常
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[MQTT] WiFi断开，等待重连...");
            delay(1000);
            continue;
        }

        if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
            Serial.println("[MQTT] 已连接！");
            mqtt.subscribe(MQTT_TOPIC_TTS_AUDIO);       // 完整音频（备用）
            mqtt.subscribe("blindstick/tts/control");   // 流式TTS控制
            mqtt.subscribe("blindstick/tts/stream/+");  // 流式TTS音频
            mqtt.subscribe(MQTT_TOPIC_NAV_STEPS);
            mqtt.subscribe(MQTT_TOPIC_TTS_REQ);
            Serial.printf("[MQTT] 已订阅主题:\n");
            Serial.printf("  - %s (完整音频-备用)\n", MQTT_TOPIC_TTS_AUDIO);
            Serial.printf("  - blindstick/tts/control (流式TTS控制)\n");
            Serial.printf("  - blindstick/tts/stream/+ (流式TTS音频)\n");
            Serial.printf("  - %s (导航步骤)\n", MQTT_TOPIC_NAV_STEPS);
            Serial.printf("  - %s (TTS请求)\n", MQTT_TOPIC_TTS_REQ);
            retryCount = 0;
        } else {
            int rc = mqtt.state();
            Serial.printf("[MQTT] 连接失败 rc=%d, ", rc);
            switch (rc) {
                case -4: Serial.print("(连接超时)"); break;
                case -3: Serial.print("(连接丢失)"); break;
                case -2: Serial.print("(连接失败-检查网络和证书)"); break;
                case -1: Serial.print("(断开连接)"); break;
                case 1:  Serial.print("(协议版本错误)"); break;
                case 2:  Serial.print("(非法ClientID)"); break;
                case 3:  Serial.print("(服务器不可用)"); break;
                case 4:  Serial.print("(用户名/密码错误)"); break;
                case 5:  Serial.print("(未授权)"); break;
                default: Serial.print("(未知错误)"); break;
            }
            Serial.println("，5秒后重试...");

            // 断开现有连接，清理状态
            mqtt.disconnect();
            espClient.stop();

            retryCount++;
            // 每5次重试重置网络测试标志
            if (retryCount >= 5) {
                Serial.println("[MQTT] 重试次数过多，重置网络...");
                network_tested = false;
                retryCount = 0;
            }
            delay(5000);  // 增加重试间隔到5秒
        }
    }
}

// ==================== MQTT 消息回调（支持流式TTS）====================
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("[MQTT] 收到 [%s] 长度=%d\n", topic, length);

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

    // ===== 测试模式：接收TTS返回音频 =====
    if (strncmp(topic, "blindstick/test/tts/", 20) == 0) {
        handleTestAudio(topic, payload, length);
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
        // 【修复】不再直接播放，而是将音频复制到队列，由 TTSPlayerTask 播放
        Serial.printf("[MQTT] 收到TTS音频: %d字节，将入队播放\n", length);

        // 【关键调试】检查音频长度是否合理
        if (length < 1000) {
            Serial.printf("[MQTT] 警告: 音频数据太短(%d字节)，可能被截断!\n", length);
            Serial.println("[MQTT] 预期应该是 60000+ 字节");
        } else if (length > 100000) {
            Serial.printf("[MQTT] 警告: 音频数据异常大(%d字节)\n", length);
        }

        if (length < 1000 || length >= TTS_AUDIO_BUF_SIZE) {
            Serial.printf("[MQTT] 音频长度无效: %d (范围: 1000-%d)\n", length, TTS_AUDIO_BUF_SIZE);
            return;
        }

        // 【关键调试】检查WAV头
        if (length > 44) {
            Serial.printf("[MQTT] WAV头检查: %c%c%c%c\n", payload[0], payload[1], payload[2], payload[3]);
            uint32_t wavSize = *(uint32_t*)(payload + 4);
            Serial.printf("[MQTT] WAV声明大小: %d 字节\n", wavSize + 8);
            Serial.printf("[MQTT] 实际接收大小: %d 字节\n", length);
            if (wavSize + 8 != length) {
                Serial.println("[MQTT] 警告: WAV大小与实际接收不符，数据可能截断!");
            }
        }

        // 暂停语音识别，释放内存
        if (VoiceTaskHandle != NULL) {
            vTaskSuspend(VoiceTaskHandle);
            webSocket.disconnect();
            Serial.println("[TTS] 语音识别已暂停");
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

            // 完整音频直接播放（备用方案，兼容旧版）
            Serial.println("[TTS] 收到完整音频，直接播放");
            // 跳过WAV头
            int offset = 0;
            if (length > 44 && audio_buf[0] == 'R' && audio_buf[1] == 'I') {
                offset = 44;
            }
            playPcmData(audio_buf + offset, length - offset);
            free(audio_buf);
            Serial.println("[TTS] 播放完成");

            // 恢复语音识别
            if (VoiceTaskHandle != NULL) {
                vTaskResume(VoiceTaskHandle);
                Serial.println("[TTS] 语音识别已恢复");
            }
        }

    } else if (strcmp(topic, MQTT_TOPIC_NAV_STEPS) == 0) {
        StaticJsonDocument<4096> doc;
        DeserializationError err = deserializeJson(doc, payload, length);
        if (!err && doc["status"] == "ok") {
            JsonArray steps = doc["steps"];
            nav_total_steps = 0;
            for (const char* step : steps) {
                nav_steps[nav_total_steps] = String(step);
                nav_total_steps++; if (nav_total_steps >= 10) break;
            }
            current_step_idx = 0; current_progress = 0; nav_active = true;
            if (doc.containsKey("destination"))
                Serial.printf("[MQTT] 导航 → %s，%d步\n", doc["destination"].as<const char*>(), nav_total_steps);
            else Serial.printf("[MQTT] 导航 %d步\n", nav_total_steps);
        }

    } else if (strcmp(topic, MQTT_TOPIC_TTS_REQ) == 0) {
        // 收到TTS请求 - 转发给代理服务器进行流式合成
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, payload, length);
        if (!err && doc.containsKey("text")) {
            const char* text = doc["text"];
            int priority = doc["priority"] | PRIO_NORMAL;
            Serial.printf("[MQTT-TTS] 转发TTS请求: %s (优先级=%d)\n", text, priority);

            // 直接在这里处理流式TTS请求，发送给代理服务器
            StaticJsonDocument<256> reqDoc;
            reqDoc["text"] = text;
            reqDoc["priority"] = priority;
            char buf[256];
            size_t len = serializeJson(reqDoc, buf, sizeof(buf));
            mqtt.publish("blindstick/tts/request", buf, len);
        }
    }
}

    // 全局变量
bool last_blocked = false;
unsigned long last_alert_time = 0;
float last_alert_dist = 0;
#define ALERT_INTERVAL_MS 5000  // 障碍物告警间隔 5 秒（从3秒增加到5秒）
#define ALERT_DIST_CHANGE 50    // 距离变化超过50cm才重新播报（从30cm增加到50cm）

// ==================== 辅助函数：使用PSRAM或普通内存分配 ====================
void* allocateBuffer(size_t size) {
    if (ESP.getPsramSize() > 0) {
        return ps_malloc(size);
    } else {
        return malloc(size);
    }
}

static char json_buffer[384];

// ==================== 通过 MQTT 发布传感器数据 ====================
void publishSensorData() {
    if (!mqtt.connected()) return;

    // 使用最小化的 JSON
    StaticJsonDocument<384> doc;
    doc["device_id"] = "blind_stick_001";
    JsonObject radar = doc.createNestedObject("radar");
    radar["f"]  = dir_smt[0];  // 正前
    radar["fl"] = dir_smt[2];  // 前左
    radar["fr"] = dir_smt[1];  // 前右
    radar["l"]  = dir_smt[4];  // 左侧
    radar["r"]  = dir_smt[3];  // 右侧
    doc["blocked"] = is_blocked;
    doc["nav"] = nav_active;
    JsonObject gps = doc.createNestedObject("gps");
    gps["lat"] = gps_lat;
    gps["lng"] = gps_lng;
    gps["sats"] = gps_satellites;

    size_t n = serializeJson(doc, json_buffer, sizeof(json_buffer));
    if (n == 0 || n >= sizeof(json_buffer)) {
        Serial.println("[MQTT] JSON序列化失败");
        return;
    }

    // 调试：打印发送的雷达值和JSON内容
    static unsigned long last_debug = 0;
    if (millis() - last_debug > 1000) {
        Serial.printf("[雷达发送] F:%.1f FL:%.1f FR:%.1f L:%.1f R:%.1f | JSON:%s\n",
                      dir_smt[0], dir_smt[2], dir_smt[1], dir_smt[4], dir_smt[3],
                      json_buffer);
        last_debug = millis();
    }

    bool ok = mqtt.publish(MQTT_TOPIC_SENSORS, json_buffer, n);
    /*if (ok) Serial.printf("[MQTT] 已发布 %d bytes\n", n);
    else Serial.println("[MQTT] 发布失败");*/
}

/**
 * 障碍物检测和播报 - 使用流式TTS
 */
void checkObstacleAndAlert() {
    // 获取当前最小距离和方向
    float min_dist = 400.0f;
    const char* direction = "前方";

    if (dir_smt[0] < min_dist) { min_dist = dir_smt[0]; direction = "正前方"; }
    if (dir_smt[1] < min_dist) { min_dist = dir_smt[1]; direction = "右前方"; }
    if (dir_smt[2] < min_dist) { min_dist = dir_smt[2]; direction = "左前方"; }
    if (dir_smt[3] < min_dist) { min_dist = dir_smt[3]; direction = "右侧"; }
    if (dir_smt[4] < min_dist) { min_dist = dir_smt[4]; direction = "左侧"; }

    unsigned long now = millis();

    // 如果有障碍物且距离小于阈值
    if (min_dist < ALERT_DIST_CM) {
        bool should_alert = false;

        if (!last_blocked) {
            should_alert = true;
        } else if (now - last_alert_time > ALERT_INTERVAL_MS) {
            should_alert = true;
        } else if (fabs(min_dist - last_alert_dist) > ALERT_DIST_CHANGE) {
            should_alert = true;
        }

        if (should_alert) {
            // 构建告警文本并通过流式TTS发送
            char alertText[128];
            if (min_dist < 50) {
                snprintf(alertText, sizeof(alertText), "%s%.0f厘米有障碍物，请立即避让", direction, min_dist);
            } else if (min_dist < 100) {
                snprintf(alertText, sizeof(alertText), "%s%.0f厘米有障碍物，请注意避让", direction, min_dist);
            } else if (min_dist < 200) {
                snprintf(alertText, sizeof(alertText), "%s%.0f厘米有障碍物", direction, min_dist);
            } else {
                snprintf(alertText, sizeof(alertText), "%s%.0f米有障碍物", direction, min_dist / 100);
            }

            Serial.printf("[障碍物播报] %s\n", alertText);

            // 发送流式TTS请求
            StaticJsonDocument<256> doc;
            doc["text"] = alertText;
            doc["priority"] = PRIO_HIGH;  // 高优先级
            char buf[256];
            size_t len = serializeJson(doc, buf, sizeof(buf));
            mqtt.publish("blindstick/tts/request", buf, len);

            last_alert_time = now;
            last_alert_dist = min_dist;
        }
        last_blocked = true;
    } else {
        last_blocked = false;
    }
}
// ==================== Core 0 守护线程 ====================
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
        smartAvoidDecision();

        if (WiFi.status() == WL_CONNECTED) {
            // 确保 MQTT 连接成功
            if (!mqtt.connected()) {
                mqtt_reconnect();
            }

        if (mqtt.connected()) {
                mqtt.loop();  // 保活

                // 障碍物检测和语音告警（使用流式TTS）
                checkObstacleAndAlert();

                if (now - lastUpload >= UPLOAD_INTERVAL_MS) {
                    lastUpload = now;
                    publishSensorData();
                }
            }
        }
        vTaskDelay(15 / portTICK_PERIOD_MS);
    }
}

// ==================== Core 1 导航任务 ====================
void NavigationTask(void* pvParameters) {
    while (true) {
        int total = nav_total_steps;
        if (total > 0 && current_step_idx < total) {
            if (current_progress < 100) {
                if (is_blocked || is_ai_talking) {
                    vTaskDelay(200 / portTICK_PERIOD_MS); continue;
                }
                vTaskDelay(1000 / portTICK_PERIOD_MS);
                current_progress += 10;
            } else {
                current_progress = 0; current_step_idx++;
                if (current_step_idx >= total) {
                    nav_active = false;
                    nav_total_steps = 1;
                    nav_steps[0] = "导航完成，请说出新目的地";
                    Serial.println("[导航] 路线已完成");
                }
                vTaskDelay(1000 / portTICK_PERIOD_MS);
            }
        } else {
            current_step_idx = 0; current_progress = 0;
            vTaskDelay(1000 / portTICK_PERIOD_MS);
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// ==================== I2S 初始化 ====================
void i2s_init() {
    Serial.printf("[I2S] 麦克风引脚配置: SCK=%d, WS=%d, SD=%d\n", I2S_SCK_PIN, I2S_WS_PIN, I2S_SD_PIN);

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
        Serial.printf("[I2S] 驱动安装失败: %d (", err);
        if (err == ESP_ERR_INVALID_ARG) Serial.print("参数错误");
        else if (err == ESP_ERR_NO_MEM) Serial.print("内存不足");
        else if (err == ESP_ERR_INVALID_STATE) Serial.print("状态错误-可能已初始化");
        else Serial.print("未知错误");
        Serial.println(")");
        return;
    }
    err = i2s_set_pin(I2S_PORT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[I2S] 引脚设置失败: %d\n", err);
        return;
    }
    Serial.println("[I2S] 麦克风初始化成功");
}

void i2s_out_init() {
    Serial.printf("[I2S-OUT] 扬声器引脚配置: BCK=%d, WS=%d, DATA=%d\n", I2S_BCK_PIN, I2S_WS_OUT_PIN, I2S_DATA_PIN);

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
        Serial.printf("[I2S-OUT] 驱动安装失败: %d\n", err);
        return;
    }
    err = i2s_set_pin(I2S_PORT_OUT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[I2S-OUT] 引脚设置失败: %d\n", err);
        return;
    }

    Serial.println("[I2S-OUT] MAX98357功放初始化成功");
}

// 简单的启动提示音
void playLocalStartupTone() {
    Serial.println("[启动提示] 播放测试音...");
    const int sample_rate = 16000;
    const int num_samples = sample_rate / 2; // 0.5秒
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

// ==================== 流式语音识别（WebSocket）====================

/**
 * WebSocket事件回调
 */
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length) {
    switch (type) {
        case WStype_DISCONNECTED: {
            Serial.println("[ASR-WS] 断开连接");
            asrConnected = false;
            break;
        }

        case WStype_CONNECTED: {
            Serial.println("[ASR-WS] 已连接，发送开始帧...");
            asrConnected = true;
            asrFinished = false;
            asrResult = "";

            // 发送开始参数帧
            StaticJsonDocument<512> doc;
            doc["type"] = "START";
            JsonObject data = doc.createNestedObject("data");
            data["appid"] = 123607377;
            data["appkey"] = BAIDU_API_KEY.c_str();
            data["dev_pid"] = 15372;  // 中文普通话，加强标点
            data["cuid"] = "esp32_blindstick";
            data["format"] = "pcm";
            data["sample"] = 16000;

            String startMsg;
            serializeJson(doc, startMsg);
            webSocket.sendTXT(startMsg);
            Serial.println("[ASR-WS] 开始帧已发送");
            break;
        }

        case WStype_TEXT: {
            // 收到识别结果
            String response((char*)payload);
            Serial.printf("[ASR-WS] 收到: %s\n", response.c_str());

            StaticJsonDocument<1024> respDoc;
            DeserializationError error = deserializeJson(respDoc, response);

            if (!error) {
                const char* msgType = respDoc["type"] | "";
                if (strcmp(msgType, "FIN_TEXT") == 0 && respDoc["err_no"] == 0) {
                    asrResult = respDoc["result"].as<String>();
                    Serial.printf("[ASR-WS] 最终结果: %s\n", asrResult.c_str());
                } else if (strcmp(msgType, "MID_TEXT") == 0) {
                    // 临时结果，可以显示但不做处理
                    Serial.printf("[ASR-WS] 临时结果: %s\n", respDoc["result"].as<const char*>());
                }
            }
            break;
        }

        case WStype_ERROR: {
            Serial.println("[ASR-WS] 错误");
            asrConnected = false;
            break;
        }

        default:
            break;
    }
}

/**
 * 发送音频数据块
 */
void sendAudioChunk() {
    if (!asrConnected) return;

    // 读取160ms音频数据 (5120字节)
    size_t bytesRead = 0;
    uint8_t buffer[ASR_CHUNK_SIZE];

    esp_err_t err = i2s_read(I2S_PORT, buffer, ASR_CHUNK_SIZE, &bytesRead, 50 / portTICK_PERIOD_MS);

    if (err == ESP_OK && bytesRead > 0) {
        // 发送二进制音频帧
        webSocket.sendBIN(buffer, bytesRead);
    }
}

/**
 * 停止流式识别
 */
void stopStreamingASR() {
    if (asrConnected) {
        // 发送结束帧
        String finishMsg = "{\"type\":\"FINISH\"}";
        webSocket.sendTXT(finishMsg);
        Serial.println("[ASR-WS] 结束帧已发送");

        // 等待最终结果
        unsigned long start = millis();
        while (millis() - start < 2000) {
            webSocket.loop();
            if (asrResult.length() > 0) break;
            delay(10);
        }

        webSocket.disconnect();
        asrConnected = false;
    }
}

/**
 * 开始流式识别
 */
void startStreamingASR() {
    // 生成随机sn
    String sn = "esp32-" + String(millis()) + "-" + String(random(1000, 9999));

    // 连接WebSocket
    String wsUrl = "/realtime_asr?sn=" + sn;
    Serial.printf("[ASR-WS] 连接地址: wss://vop.baidu.com:443%s\n", wsUrl.c_str());

    webSocket.beginSSL("vop.baidu.com", 443, wsUrl.c_str(), "", "wss");
    webSocket.onEvent(webSocketEvent);
    webSocket.setReconnectInterval(5000);
    // 跳过证书验证（开发阶段）
    webSocket.setAuthorization("");

    Serial.println("[ASR-WS] 正在连接...");

    // 等待连接成功
    unsigned long start = millis();
    while (!asrConnected && millis() - start < 10000) {
        webSocket.loop();
        delay(10);
    }

    if (!asrConnected) {
        Serial.println("[ASR-WS] 连接超时");
        return;
    }

    Serial.println("[ASR-WS] 开始录音...");
    asrResult = "";
    asrFinished = false;
}

/**
 * 流式语音识别任务（开机就开始，边录边传）
 */
void VoiceRecognitionTask(void* pvParameters) {
    Serial.println("[语音识别] 任务已启动，等待WiFi...");

    // 等待WiFi连接
    int waitCount = 0;
    while (WiFi.status() != WL_CONNECTED && waitCount < 60) {
        vTaskDelay(1000 / portTICK_PERIOD_MS);
        waitCount++;
        if (waitCount % 10 == 0) {
            Serial.printf("[语音识别] 等待WiFi... %d秒\n", waitCount);
        }
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[语音识别] WiFi未连接，任务退出");
        vTaskDelete(NULL);
        return;
    }

    Serial.println("[语音识别] WiFi已连接，启动流式语音识别...");

    // 初始化WebSocket
    webSocket.onEvent(webSocketEvent);
    Serial.println("[语音识别] WebSocket事件处理器已注册");

    while (true) {
        if (WiFi.status() != WL_CONNECTED) {
            Serial.println("[语音识别] WiFi断开，等待重连...");
            vTaskDelay(5000 / portTICK_PERIOD_MS);
            continue;
        }

        Serial.println("[语音识别] 开始新的识别会话...");

        // 开始一次识别会话
        startStreamingASR();

        if (!asrConnected) {
            Serial.println("[语音识别] WebSocket连接失败，尝试REST API备选方案...");
            // 使用REST API识别（和Python后端一样）
            String result = doRESTASR();
            if (result.length() > 0) {
                Serial.printf("[语音识别] REST API识别结果: %s\n", result.c_str());
                handleVoiceCommand(result.c_str());
            }
            vTaskDelay(3000 / portTICK_PERIOD_MS);
            continue;
        }

        // 持续录音并发送，最多10秒
        unsigned long sessionStart = millis();
        int chunkCount = 0;

        while (asrConnected && millis() - sessionStart < 10000) {
            sendAudioChunk();
            webSocket.loop();
            chunkCount++;

            // 每160ms发送一帧
            vTaskDelay(160 / portTICK_PERIOD_MS);

            // 检查是否已有结果
            if (asrResult.length() > 0) {
                Serial.printf("[语音识别] 已收到结果，提前结束录音\n");
                break;
            }
        }

        Serial.printf("[语音识别] 会话结束，发送了%d帧数据\n", chunkCount);

        // 停止识别
        stopStreamingASR();

        // 处理识别结果
        if (asrResult.length() > 0) {
            Serial.printf("[语音识别] 识别结果: %s\n", asrResult.c_str());
            handleVoiceCommand(asrResult.c_str());
        } else {
            Serial.println("[语音识别] 未识别到语音");
        }

        // 短暂间隔后开始下一次识别
        vTaskDelay(500 / portTICK_PERIOD_MS);
    }
}

/**
 * REST API语音识别备选方案（与Python后端一致）
 * 录音2秒 -> Base64编码 -> POST到百度ASR API -> 返回结果
 */
String doRESTASR() {
    Serial.println("[ASR-REST] 开始录音...");

    // 检查PSRAM
    size_t psramSize = ESP.getPsramSize();
    size_t freePsram = ESP.getFreePsram();
    Serial.printf("[ASR-REST] PSRAM: 总共%dKB, 可用%dKB\n", psramSize/1024, freePsram/1024);

    // 录音2秒 (64KB)
    const int RECORD_SIZE = 16000 * 2 * 2;
    uint8_t* buffer = NULL;

    // 优先使用PSRAM
    if (psramSize > 0) {
        buffer = (uint8_t*)ps_malloc(RECORD_SIZE);
        Serial.println("[ASR-REST] 使用PSRAM分配录音缓冲区");
    } else {
        buffer = (uint8_t*)malloc(RECORD_SIZE);
        Serial.println("[ASR-REST] 使用普通内存分配录音缓冲区");
    }

    if (!buffer) {
        Serial.println("[ASR-REST] 内存分配失败");
        return "";
    }

    // 录音
    size_t totalRead = 0;
    unsigned long startTime = millis();
    while (millis() - startTime < 2000 && totalRead < RECORD_SIZE) {
        size_t bytesRead = 0;
        i2s_read(I2S_PORT, buffer + totalRead, RECORD_SIZE - totalRead, &bytesRead, 50);
        totalRead += bytesRead;
    }

    Serial.printf("[ASR-REST] 录音完成: %d字节\n", totalRead);

    // 获取Token - 直接请求
    String token = "";
    {
        WiFiClientSecure tokenClient;
        tokenClient.setInsecure();
        tokenClient.setTimeout(10000);
        HTTPClient tokenHttp;
        String url = String("https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials&client_id=") + BAIDU_API_KEY + "&client_secret=" + BAIDU_SECRET_KEY;
        if (tokenHttp.begin(tokenClient, url)) {
            tokenHttp.setTimeout(10000);
            int code = tokenHttp.GET();
            if (code == 200) {
                String resp = tokenHttp.getString();
                StaticJsonDocument<512> doc;
                if (!deserializeJson(doc, resp)) {
                    token = doc["access_token"].as<String>();
                }
            }
            tokenHttp.end();
        }
    }

    if (token.length() == 0) {
        Serial.println("[ASR-REST] 无法获取Token");
        free(buffer);
        return "";
    }

    // Base64编码
    String base64Audio = base64Encode(buffer, totalRead);
    free(buffer);

    // 发送ASR请求（与Python一致）
    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;
    if (!http.begin(client, "https://vop.baidu.com/server_api")) {
        return "";
    }

    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);

    // 构建JSON（与Python一致）
    StaticJsonDocument<4096> doc;
    doc["format"] = "pcm";
    doc["rate"] = 16000;
    doc["channel"] = 1;
    doc["cuid"] = "esp32_blindstick";
    doc["token"] = token;
    doc["dev_pid"] = 1537;  // 与Python一致：中文普通话，弱标点
    doc["speech"] = base64Audio;
    doc["len"] = totalRead;

    String jsonPayload;
    serializeJson(doc, jsonPayload);

    Serial.println("[ASR-REST] 发送识别请求...");
    int httpCode = http.POST(jsonPayload);

    String result = "";
    if (httpCode == 200) {
        String response = http.getString();
        StaticJsonDocument<1024> respDoc;
        DeserializationError error = deserializeJson(respDoc, response);

        if (!error && respDoc["err_no"] == 0) {
            JsonArray results = respDoc["result"];
            if (results.size() > 0) {
                result = results[0].as<String>();
            }
        } else {
            Serial.printf("[ASR-REST] 识别错误: %d\n", (int)respDoc["err_no"]);
        }
    } else {
        Serial.printf("[ASR-REST] HTTP错误: %d\n", httpCode);
    }

    http.end();
    return result;
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
    }

    return encoded;
}

// ==================== setup / loop ====================
void setup() {
    Serial.begin(115200);
    randomSeed(millis());  // 初始化随机数种子

    // ===== PSRAM 初始化（必须在内存分配前完成）=====
    Serial.println("[系统] 初始化PSRAM...");
    if (psramInit()) {
        size_t psram_total = ESP.getPsramSize();
        size_t psram_free = ESP.getFreePsram();
        Serial.printf("[PSRAM] 初始化成功！总计:%dKB, 可用:%dKB\n",
                     psram_total/1024, psram_free/1024);
    } else {
        Serial.println("[PSRAM] 初始化失败或未安装PSRAM");
    }

    pinMode(MOTOR_IN1, OUTPUT); pinMode(MOTOR_IN2, OUTPUT); pinMode(MOTOR_PWM, OUTPUT);
    motorControl(0);
    pinMode(RADAR_M_CTR_PIN, OUTPUT);
    digitalWrite(RADAR_M_CTR_PIN, HIGH);
    Serial.println("[雷达] 电机启动");

    nav_total_steps = 1;
    nav_steps[0] = "请说出目的地，如：带我去天安门";
    current_step_idx = 0; current_progress = 0; nav_active = false;

    audioMutex = xSemaphoreCreateMutex();
    if (audioMutex) Serial.println("[系统] 互斥锁已创建");

    // 初始化流式TTS
    initStreamingTTS();

    i2s_out_init();
    // I2S 麦克风初始化（必须在 VoiceAssistantTask 启动前完成）
    i2s_init();
    Serial.println("[I2S] 麦克风初始化完成");

    // 播放测试音确认扬声器工作
    Serial.println("[测试] 播放测试音...");
    playLocalStartupTone();

    // ===== 麦克风硬件诊断测试 =====
    Serial.println("\n[诊断] 开始麦克风硬件测试...");
    delay(100);
    uint8_t test_buf[512];
    size_t bytes_read = 0;
    int test_non_zero = 0;
    int test_clipped = 0;

    // 连续读取5次，每次100ms
    for (int test = 0; test < 5; test++) {
        esp_err_t err = i2s_read(I2S_PORT, test_buf, sizeof(test_buf), &bytes_read, 100);
        if (err == ESP_OK && bytes_read > 0) {
            int16_t* samples = (int16_t*)test_buf;
            for (int i = 0; i < bytes_read / 2; i++) {
                if (samples[i] != 0 && samples[i] != -1) test_non_zero++;
                if (samples[i] > 30000 || samples[i] < -30000) test_clipped++;
            }
            Serial.printf("[诊断] 测试%d: 读取%d字节, 前10字节: ", test+1, bytes_read);
            for (int i = 0; i < 10 && i < bytes_read; i++) Serial.printf("%02X ", test_buf[i]);
            Serial.println();
        } else {
            Serial.printf("[诊断] 测试%d: 读取失败 err=%d\n", test+1, err);
        }
        delay(100);
    }
    Serial.printf("[诊断] 总计: 非零样本=%d, 削顶=%d\n", test_non_zero, test_clipped);
    if (test_non_zero == 0) {
        Serial.println("[诊断] ⚠️ 警告: 麦克风没有输出数据!");
    } else {
        Serial.println("[诊断] ✅ 麦克风硬件正常");
    }
    Serial.println();

    // 启动 TTS 播放器任务 - 流式TTS已集成到MQTT回调，无需单独任务
    // xTaskCreatePinnedToCore(TTSPlayerTask, "TTSPlayer", 8192, NULL, 4, &TTSPlayerTaskHandle, 1);
    // Serial.println("[系统] TTS播放任务已启动");
    // delay(100);

    // 连接WiFi
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    unsigned long wifi_start = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - wifi_start > 30000) {
            Serial.println("[WiFi] 连接超时30秒，离线模式继续启动");
            break;
        }
        delay(500);
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] 已连接！IP: %s\n", WiFi.localIP().toString().c_str());

        // 同步时间（TLS证书验证需要）
        Serial.println("[NTP] 同步网络时间...");
        configTime(8 * 3600, 0, "ntp.ntsc.ac.cn", "cn.pool.ntp.org");
        struct tm timeinfo;
        int ntp_retry = 0;
        while (!getLocalTime(&timeinfo) && ntp_retry < 10) {
            delay(500);
            ntp_retry++;
        }
        if (ntp_retry < 10) {
            Serial.printf("[NTP] 时间同步成功: %04d-%02d-%02d %02d:%02d:%02d\n",
                          timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                          timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
        } else {
            Serial.println("[NTP] 时间同步失败，继续尝试连接...");
        }

        // 【关键】先配置TLS客户端，再初始化MQTT
        Serial.println("[TLS] 配置安全客户端...");
        espClient.setInsecure();  // 跳过证书验证
        espClient.setHandshakeTimeout(12);  // 12秒握手超时（测试显示需要6秒+）

        // 初始化 MQTT
        mqtt.setServer(MQTT_BROKER, MQTT_PORT);
        mqtt.setCallback(mqtt_callback);

        // 连接MQTT（非阻塞重试）
        mqtt_reconnect();

        // WiFi 已连接，使用MQTT代理播报启动成功
        delay(500);
        StaticJsonDocument<256> doc;
        doc["text"] = "导盲杖系统启动成功，欢迎使用";
        doc["priority"] = PRIO_NORMAL;
        char buf[256];
        size_t len = serializeJson(doc, buf, sizeof(buf));
        mqtt.publish("blindstick/tts/request", buf, len);
        Serial.println("[系统] 启动播报请求已发送");
    } else {
        Serial.println("[WiFi] 离线模式，MQTT不可用，跳过启动播报");
    }

    Serial.println("[系统] 等待语音输入目的地...");

    // xTaskCreatePinnedToCore(RadarMotorUploadTask, "RadarTask", 8192, NULL, 3, &RadarTaskHandle, 0);
    // xTaskCreatePinnedToCore(NavigationTask, "NavTask", 2048, NULL, 1, &NavTaskHandle, 1);
    // xTaskCreatePinnedToCore(VoiceRecognitionTask, "VoiceRecTask", 8192, NULL, 2, &VoiceTaskHandle, 1);

    // ===== 测试模式：录音回环 =====
    Serial.println("\n[系统] 启动测试模式：录音回环");
    xTaskCreatePinnedToCore(testRecordAndPlaybackTask, "TestRecTask", 8192, NULL, 2, &VoiceTaskHandle, 1);
    Serial.println("[系统] 按 BOOT 键开始录音测试\n");

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
 * 搜索最近的目的地
 */
bool searchNearestDestination(const char* keyword, float& outLat, float& outLng, String& outName, float& outDistance) {
    if (WiFi.status() != WL_CONNECTED) return false;

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;
    String url = "https://api.map.baidu.com/place/v2/search?query=" + urlEncode(keyword) +
                 "&region=" + urlEncode("黄石市") +
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

    // 计算当前位置
    float currentLat = gps_lat > 1.0 ? gps_lat : 30.229320;
    float currentLng = gps_lng > 1.0 ? gps_lng : 115.063977;

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

    float originLat = gps_lat > 1.0 ? gps_lat : 30.229320;
    float originLng = gps_lng > 1.0 ? gps_lng : 115.063977;

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
 */
void handleVoiceCommand(const char* text) {
    Serial.printf("[语音识别] 识别结果: %s\n", text);

    // 提取目的地
    String destination = extractDestination(text);

    if (destination.length() < 2) {
        Serial.println("[语音识别] 未提取到有效目的地");
        return;
    }

    Serial.printf("[语音识别] 目的地: %s\n", destination.c_str());

    // 搜索最近的目的地
    float destLat, destLng, distance;
    String destName;

    if (!searchNearestDestination(destination.c_str(), destLat, destLng, destName, distance)) {
        // 发送失败提示 - 使用流式TTS
        StaticJsonDocument<256> doc;
        doc["text"] = "抱歉，没有找到该地点";
        doc["priority"] = PRIO_NORMAL;
        char buf[256];
        size_t len = serializeJson(doc, buf, sizeof(buf));
        mqtt.publish("blindstick/tts/request", buf, len);
        return;
    }

    // 检查距离是否太远
    if (distance > MAX_NAVIGATION_DISTANCE) {
        char msg[128];
        snprintf(msg, sizeof(msg), "目的地%s距离您%.1f公里，距离太远，请重新选择较近的地点",
                 destName.c_str(), distance / 1000);
        StaticJsonDocument<256> doc;
        doc["text"] = msg;
        doc["priority"] = PRIO_NORMAL;
        char buf[256];
        size_t len = serializeJson(doc, buf, sizeof(buf));
        mqtt.publish("blindstick/tts/request", buf, len);
        return;
    }

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
            webSocket.disconnect();
            Serial.println("[流式TTS] 语音识别已暂停");
        }

    } else if (strcmp(type, "stream_end") == 0) {
        int segments = doc["segments"] | 0;
        Serial.printf("[流式TTS] 会话结束，共%d段\n", segments);

        if (stream_buf_used > 0 && stream_playing) {
            playPcmData(stream_buffer, stream_buf_used);
        }

        if (VoiceTaskHandle != NULL && stream_priority < PRIO_HIGH) {
            vTaskResume(VoiceTaskHandle);
        }

        stream_playing = false;
        stream_buf_used = 0;

    } else if (strcmp(type, "interrupt") == 0) {
        Serial.printf("[流式TTS] 收到打断信号\n");
        stopCurrentPlayback();
        if (VoiceTaskHandle != NULL) {
            vTaskResume(VoiceTaskHandle);
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

void announceObstacleStreaming(float distance, const char* direction) {
    char text[64];
    if (distance < 150) {
        snprintf(text, sizeof(text), "注意！前方%.0f厘米有障碍物，请立即避让！", distance);
    } else {
        snprintf(text, sizeof(text), "前方%.0f厘米有%s障碍物", distance, direction);
    }
    Serial.printf("[雷达告警] %s\n", text);

    // 发送MQTT流式TTS请求
    StaticJsonDocument<256> doc;
    doc["text"] = text;
    doc["priority"] = PRIO_HIGH;
    char buf[256];
    size_t len = serializeJson(doc, buf, sizeof(buf));
    mqtt.publish("blindstick/tts/request", buf, len);
}

// ==================== 测试模式：录音回环 ====================
// 临时测试：录音 -> MQTT发送 -> 后端ASR+TTS -> MQTT返回 -> 播放

#define TEST_RECORD_TIME_MS 2000  // 录音2秒
#define TEST_AUDIO_CHUNK_SIZE 512 // 每次发送512字节

void testRecordAndPlaybackTask(void* pvParameters) {
    Serial.println("\n========================================");
    Serial.println("  测试模式：录音回环");
    Serial.println("  按 BOOT 键开始录音...");
    Serial.println("========================================\n");

    const int bufferSize = SAMPLE_RATE * 2 * (TEST_RECORD_TIME_MS / 1000); // 64KB

    while (true) {
        // 等待按键（GPIO 0 通常是BOOT键）
        if (digitalRead(0) == LOW) {
            delay(50); // 消抖
            if (digitalRead(0) == LOW) {
                Serial.println("[测试] 开始录音...");

                // 分配缓冲区
                uint8_t* audioBuffer = (uint8_t*)ps_malloc(bufferSize);
                if (!audioBuffer) {
                    audioBuffer = (uint8_t*)malloc(bufferSize);
                }
                if (!audioBuffer) {
                    Serial.println("[测试] 内存分配失败");
                    continue;
                }

                // 录音
                size_t totalRead = 0;
                unsigned long startTime = millis();
                while (millis() - startTime < TEST_RECORD_TIME_MS && totalRead < bufferSize) {
                    size_t bytesRead = 0;
                    i2s_read(I2S_PORT, audioBuffer + totalRead, bufferSize - totalRead, &bytesRead, 50);
                    totalRead += bytesRead;
                }
                Serial.printf("[测试] 录音完成: %d 字节\n", totalRead);

                // 等待按键释放
                while (digitalRead(0) == LOW) delay(10);

                // 通过MQTT发送音频数据（分块发送）
                if (mqtt.connected()) {
                    // 1. 发送开始标记
                    StaticJsonDocument<256> startDoc;
                    startDoc["type"] = "test_record_start";
                    startDoc["size"] = totalRead;
                    startDoc["rate"] = SAMPLE_RATE;
                    char startBuf[256];
                    size_t startLen = serializeJson(startDoc, startBuf, sizeof(startBuf));
                    mqtt.publish("blindstick/test/audio", startBuf, startLen);
                    Serial.println("[测试] 发送开始标记");

                    // 2. 分块发送音频数据
                    int chunks = 0;
                    for (int offset = 0; offset < totalRead; offset += TEST_AUDIO_CHUNK_SIZE) {
                        int chunkSize = min(TEST_AUDIO_CHUNK_SIZE, (int)(totalRead - offset));
                        char topic[64];
                        snprintf(topic, sizeof(topic), "blindstick/test/audio/chunk/%d", chunks);
                        mqtt.publish(topic, audioBuffer + offset, chunkSize);
                        chunks++;
                        delay(10); // 避免发送太快
                    }
                    Serial.printf("[测试] 发送完成，共%d块\n", chunks);

                    // 3. 发送结束标记
                    StaticJsonDocument<256> endDoc;
                    endDoc["type"] = "test_record_end";
                    endDoc["chunks"] = chunks;
                    char endBuf[256];
                    size_t endLen = serializeJson(endDoc, endBuf, sizeof(endBuf));
                    mqtt.publish("blindstick/test/audio", endBuf, endLen);
                    Serial.println("[测试] 等待后端处理...");
                } else {
                    Serial.println("[测试] MQTT未连接");
                }

                free(audioBuffer);

                // 等待播放完成（后端会通过MQTT返回音频）
                Serial.println("[测试] 等待播放...\n");
            }
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// 播放接收到的测试音频
void handleTestAudio(const char* topic, byte* payload, unsigned int length) {
    // 主题格式: blindstick/test/tts/chunk/N
    if (strncmp(topic, "blindstick/test/tts/chunk/", 26) == 0) {
        Serial.printf("[测试] 收到TTS音频块: %d字节\n", length);
        playPcmData(payload, length);
    }
}

