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

// ==================== 网络参数 ====================
const char* WIFI_SSID     = "ZZY";
const char* WIFI_PASSWORD = "zzy060630";

// ==================== MQTT 参数（EMQX Cloud） ====================
const char* MQTT_BROKER   = "u72a7838.ala.asia-southeast1.emqxsl.com";
const int   MQTT_PORT     = 8883;  // MQTT over TLS（PubSubClient 不支持 WebSocket）
const char* MQTT_USER     = "blindstick";
const char* MQTT_PASSWORD = "2026";
const char* MQTT_CLIENT_ID = "blindstick_esp32_001";
const char* MQTT_TOPIC_SENSORS   = "blindstick/sensors";
const char* MQTT_TOPIC_TTS_REQ   = "blindstick/tts/request";
const char* MQTT_TOPIC_TTS_AUDIO = "blindstick/tts/audio";
const char* MQTT_TOPIC_NAV_STEPS = "blindstick/nav/steps";

WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

// TTS音频接收缓冲（处理MQTT下发的音频）
#define TTS_AUDIO_BUF_SIZE  (64 * 1024)   // 64KB，平衡内存和音频大小
volatile uint8_t tts_rx_buf[TTS_AUDIO_BUF_SIZE];
volatile int     tts_rx_len = 0;
volatile bool    tts_rx_ready = false;
volatile unsigned long tts_rx_start = 0;
#define TTS_RX_TIMEOUT_MS  5000  // TTS音频接收超时5秒

// ==================== 高德地图参数 ====================
const String AMAP_KEY     = "90a13b148658cc0a0525f959202a4063";
const String NAV_DEST_NAME    = "湖北师范大学教育大楼";
const String NAV_ORIGIN       = "115.063977,30.229320";

// ==================== 百度语音 API 配置 ====================
const String BAIDU_APP_ID         = "123607377";
const String BAIDU_API_KEY        = "Xbxnhkwb2sxtB6HbH5BUTlUG";
const String BAIDU_SECRET_KEY     = "Tw485P2BFGpPu8WeOVP6hy4S1BHqG4ON";
const String BAIDU_TTS_URL        = "https://tsn.baidu.com/text2audio";
const String BAIDU_TOKEN_URL      = "https://aip.baidubce.com/oauth/2.0/token";
const String BAIDU_ASR_WS_URL     = "wss://vop.baidu.com/realtime_asr";

String baidu_access_token = "";
unsigned long baidu_token_expire = 0;

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
// WS  → GPIO2
// SCK → GPIO1
// SD  → GPIO42
// GND → GND
// L/R → GND (接地=左声道)
#define I2S_WS_PIN      2
#define I2S_SCK_PIN     1
#define I2S_SD_PIN      42
#define I2S_PORT        I2S_NUM_0

// ==================== I2S扬声器引脚 (MAX98357) ====================
#define I2S_BCK_PIN     4
#define I2S_WS_OUT_PIN  5
#define I2S_DATA_PIN    6
#define I2S_PORT_OUT    I2S_NUM_1

#define VOLUME_GAIN     0.6

// ==================== 函数声明 ====================
String urlEncode(const char* str);
float calcDistance(float lat1, float lng1, float lat2, float lng2);
String getBaiduAccessToken();
bool baiduTTSPlay(const char* text);
bool requestTTSViaMQTT(const char* text);
void playStartupSuccess();
void announceObstacle(float distance, const char* direction);
String extractDestination(const char* text);
bool searchNearestDestination(const char* keyword, float& outLat, float& outLng, String& outName, float& outDistance);
bool planWalkingRoute(float destLat, float destLng, String& destName);
void handleVoiceCommand(const char* text);
void checkObstacleAndAlertBaidu();

// 流式语音识别相关
void VoiceRecognitionTask(void* pvParameters);
void startStreamingASR();
void stopStreamingASR();
void sendAudioChunk();
void webSocketEvent(WStype_t type, uint8_t* payload, size_t length);
String doRESTASR();  // REST API备选方案
String base64Encode(const uint8_t* data, size_t len);  // Base64编码

// 流式语音识别全局变量
WebSocketsClient webSocket;
volatile bool asrConnected = false;
volatile bool asrFinished = false;
String asrResult = "";

#define ASR_CHUNK_SIZE 5120  // 160ms PCM数据 @ 16kHz 16bit

// TTS音频分段接收
volatile int tts_segments_expected = 0;
volatile int tts_segments_received = 0;
volatile int tts_total_size = 0;
volatile int tts_received_size = 0;

// ==================== 避障阈值 ====================
#define ALERT_DIST_CM       180.0
#define FRONT_CRITICAL_CM   150.0
#define SIDE_WARNING_CM     70.0
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

// ==================== TTS 音频队列配置 ====================
enum TTS_Priority {
    TTS_PRIORITY_LOW = 0,
    TTS_PRIORITY_NORMAL = 1,
    TTS_PRIORITY_HIGH = 2
};

struct TTS_QueueItem {
    uint8_t* audio_data;
    int audio_len;
    TTS_Priority priority;
    unsigned long timestamp;
    TTS_QueueItem* next;
};

struct TTS_Queue {
    TTS_QueueItem* head;
    TTS_QueueItem* tail;
    int count;
    int max_count;
};

TTS_Queue tts_queue = {NULL, NULL, 0, 2};
volatile bool is_playing_audio = false;
volatile TTS_Priority current_playing_priority = TTS_PRIORITY_LOW;
SemaphoreHandle_t audioMutex = NULL;

// ==================== 全局状态变量 ====================
String nav_steps[10];
volatile int nav_total_steps = 0;
volatile int current_step_idx = 0;
volatile int current_progress = 0;
volatile bool nav_active = false;

volatile bool  is_blocked  = false;
volatile bool  is_ai_talking = false;

enum AudioState {
    AUDIO_IDLE   = 0,
    AUDIO_AVOID  = 1,
    AUDIO_NAV    = 2,
    AUDIO_CHAT   = 3
};
volatile AudioState audio_request_state = AUDIO_IDLE;

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

// ==================== TTS 音频队列操作函数 ====================
void tts_queue_clear() {
    while (tts_queue.head != NULL) {
        TTS_QueueItem* item = tts_queue.head;
        tts_queue.head = item->next;
        if (item->audio_data != NULL) free(item->audio_data);
        free(item);
    }
    tts_queue.tail = NULL;
    tts_queue.count = 0;
    Serial.println("[TTS队列] 已清空");
}

bool tts_queue_enqueue(uint8_t* audio_data, int audio_len, TTS_Priority priority) {
    if (tts_queue.count >= tts_queue.max_count) {
        Serial.printf("[TTS队列] 队列已满(%d)，丢弃低优先级音频\n", tts_queue.count);
        if (priority == TTS_PRIORITY_LOW) return false;
        TTS_QueueItem* prev = NULL;
        TTS_QueueItem* curr = tts_queue.head;
        while (curr != NULL) {
            if (curr->priority == TTS_PRIORITY_LOW) {
                if (prev == NULL) tts_queue.head = curr->next;
                else { prev->next = curr->next; }
                if (curr == tts_queue.tail) tts_queue.tail = prev;
                free(curr->audio_data); free(curr);
                tts_queue.count--;
                Serial.println("[TTS队列] 丢弃队列中的低优先级音频，为新音频腾出空间");
                break;
            }
            prev = curr; curr = curr->next;
        }
    }
    TTS_QueueItem* new_item = (TTS_QueueItem*)malloc(sizeof(TTS_QueueItem));
    if (!new_item) { Serial.println("[TTS队列] 内存分配失败"); return false; }
    new_item->audio_data = (uint8_t*)malloc(audio_len);
    if (!new_item->audio_data) { free(new_item); return false; }
    memcpy(new_item->audio_data, audio_data, audio_len);
    new_item->audio_len = audio_len;
    new_item->priority = priority;
    new_item->timestamp = millis();
    new_item->next = NULL;
    TTS_QueueItem* prev = NULL;
    TTS_QueueItem* curr = tts_queue.head;
    while (curr != NULL && curr->priority >= priority) { prev = curr; curr = curr->next; }
    if (prev == NULL) { new_item->next = tts_queue.head; tts_queue.head = new_item; }
    else { new_item->next = curr; prev->next = new_item; }
    if (curr == NULL) tts_queue.tail = new_item;
    tts_queue.count++;
    Serial.printf("[TTS队列] 音频入队，优先级=%d，队列长度=%d\n", priority, tts_queue.count);
    return true;
}

TTS_QueueItem* tts_queue_dequeue() {
    if (tts_queue.head == NULL) return NULL;
    TTS_QueueItem* item = tts_queue.head;
    tts_queue.head = item->next;
    if (tts_queue.head == NULL) tts_queue.tail = NULL;
    tts_queue.count--;
    return item;
}

bool tts_should_interrupt_current(TTS_Priority new_priority) {
    if (!is_playing_audio) return false;
    return new_priority > current_playing_priority;
}

const char* tts_priority_name(TTS_Priority priority) {
    switch (priority) {
        case TTS_PRIORITY_LOW: return "低(避障)";
        case TTS_PRIORITY_NORMAL: return "中(对话)";
        case TTS_PRIORITY_HIGH: return "高(导航)";
        default: return "未知";
    }
}

// ==================== TTS 播放任务 ====================
void TTSPlayerTask(void* pvParameters) {
    Serial.println("[TTS播放器] 播放任务已启动");
    is_playing_audio = false;
    current_playing_priority = TTS_PRIORITY_LOW;
    while (true) {
        if (tts_queue.head != NULL && !is_playing_audio) {
            TTS_QueueItem* item = tts_queue_dequeue();
            if (item != NULL) {
                is_playing_audio = true;
                current_playing_priority = item->priority;
                Serial.printf("[TTS播放器] 开始播放，优先级=%s，大小=%d字节\n",
                             tts_priority_name(item->priority), item->audio_len);

                uint8_t* pcm_data = item->audio_data;
                int pcm_len = item->audio_len;

                // 检查是否是WAV格式（有44字节头）
                int data_offset = 0;
                if (pcm_len > 44 &&
                    pcm_data[0] == 'R' && pcm_data[1] == 'I' &&
                    pcm_data[2] == 'F' && pcm_data[3] == 'F') {
                    data_offset = 44;
                    Serial.println("[TTS播放器] 检测到WAV格式，跳过44字节头");
                } else {
                    Serial.println("[TTS播放器] 原始PCM数据，无WAV头");
                }

                if (pcm_len > data_offset) {
                    pcm_data += data_offset;
                    pcm_len -= data_offset;

                    Serial.printf("[TTS播放器] 实际PCM数据: %d字节\n", pcm_len);

                    int16_t* temp_buffer = (int16_t*)malloc(pcm_len);
                    if (temp_buffer) {
                        int16_t* src_samples = (int16_t*)pcm_data;
                        int num_samples = pcm_len / 2;

                        // 应用音量增益
                        for (int i = 0; i < num_samples; i++) {
                            int32_t sample = (int32_t)(src_samples[i] * VOLUME_GAIN);
                            if (sample > 32767) sample = 32767;
                            if (sample < -32768) sample = -32768;
                            temp_buffer[i] = (int16_t)sample;
                        }

                        size_t bytes_written = 0;
                        int total_written = 0;
                        const int CHUNK_SIZE = 4096;

                        Serial.println("[TTS播放器] 开始I2S写入...");
                        while (total_written < pcm_len) {
                            int to_write = min(CHUNK_SIZE, pcm_len - total_written);
                            i2s_write(I2S_PORT_OUT, ((uint8_t*)temp_buffer) + total_written,
                                     to_write, &bytes_written, portMAX_DELAY);
                            if (bytes_written > 0) {
                                total_written += bytes_written;
                            } else {
                                Serial.println("[TTS播放器] I2S写入失败");
                                break;
                            }
                        }

                        Serial.printf("[TTS播放器] I2S写入完成: %d字节\n", total_written);

                        free(temp_buffer);
                        i2s_zero_dma_buffer(I2S_PORT_OUT);

                        // 等待播放完成
                        int audio_duration_ms = (pcm_len / 32000.0) * 1000;
                        Serial.printf("[TTS播放器] 等待 %d ms...\n", audio_duration_ms);
                        delay(audio_duration_ms + 100);
                        Serial.println("[TTS播放器] 播放完成");
                    } else {
                        Serial.println("[TTS播放器] 内存不足，无法分配播放缓冲区");
                    }
                } else {
                    Serial.println("[TTS播放器] 音频数据太短");
                }

                free(item->audio_data);
                free(item);
                is_playing_audio = false;
                current_playing_priority = TTS_PRIORITY_LOW;
            }
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

bool play_tts_audio(uint8_t* audio_data, int audio_len, TTS_Priority priority) {
    if (tts_should_interrupt_current(priority)) {
        Serial.printf("[TTS] %s优先级音频打断当前播放\n", priority > current_playing_priority ? "更高" : "相同");
        i2s_zero_dma_buffer(I2S_PORT_OUT);
        is_playing_audio = false;
        delay(50);
    }
    return tts_queue_enqueue(audio_data, audio_len, priority);
}

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

    // 配置MQTT客户端
    mqtt.setSocketTimeout(5);
    mqtt.setKeepAlive(30);
    mqtt.setBufferSize(75000);  // 减少到75KB，足够接收60KB分段+MQTT开销
    mqtt.setCallback(mqtt_callback);

    int retryCount = 0;
    while (!mqtt.connected()) {
        Serial.printf("[MQTT] 尝试连接 #%d broker=%s:%d...\n", retryCount + 1, MQTT_BROKER, MQTT_PORT);
        Serial.printf("[MQTT] ClientID=%s, User=%s\n", MQTT_CLIENT_ID, MQTT_USER);

        // 设置TLS客户端超时
        espClient.setTimeout(5000); // 5秒TLS握手超时

        if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
            Serial.println("[MQTT] 已连接！");
            mqtt.subscribe(MQTT_TOPIC_TTS_AUDIO);
            mqtt.subscribe("blindstick/tts/segments");
            mqtt.subscribe("blindstick/tts/pcm/+");  // 使用通配符订阅所有分段
            mqtt.subscribe(MQTT_TOPIC_NAV_STEPS);
            mqtt.subscribe(MQTT_TOPIC_TTS_REQ);
            Serial.printf("[MQTT] 已订阅主题:\n");
            Serial.printf("  - %s (TTS音频)\n", MQTT_TOPIC_TTS_AUDIO);
            Serial.printf("  - blindstick/tts/segments (分段通知)\n");
            Serial.printf("  - blindstick/tts/pcm/+ (分段音频)\n");
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
            Serial.println("，3秒后重试...");
            retryCount++;
            // 每5次重试重置网络测试标志
            if (retryCount >= 5) {
                Serial.println("[MQTT] 重试次数过多，重置网络...");
                network_tested = false;
                retryCount = 0;
            }
            delay(3000);
        }
    }
}

// ==================== MQTT 消息回调 ====================
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    Serial.printf("[MQTT] 收到 [%s] 长度=%d\n", topic, length);

    if (strcmp(topic, MQTT_TOPIC_TTS_AUDIO) == 0) {
        Serial.printf("[MQTT] 收到TTS音频: %d字节\n", length);
        if (length > 1000 && length < TTS_AUDIO_BUF_SIZE) {
            // 暂停语音识别，释放内存
            if (VoiceTaskHandle != NULL) {
                vTaskSuspend(VoiceTaskHandle);
                webSocket.disconnect();
                Serial.println("[TTS] 语音识别已暂停");
            }

            // 直接播放，不缓冲
            Serial.println("[TTS] 开始播放...");

            // 检查WAV头
            int offset = 0;
            if (length > 44 && payload[0]=='R' && payload[1]=='I' && payload[2]=='F' && payload[3]=='F') {
                offset = 44;
                Serial.println("[TTS] 跳过WAV头");
            }

            // 清空I2S缓冲区
            i2s_zero_dma_buffer(I2S_PORT_OUT);

            // 写入I2S（分块写入，避免DMA缓冲区溢出）
            const int CHUNK_SIZE = 4096;
            int total_written = 0;
            size_t written = 0;

            while (total_written < (int)(length - offset)) {
                int to_write = min(CHUNK_SIZE, (int)(length - offset - total_written));
                i2s_write(I2S_PORT_OUT, payload + offset + total_written, to_write, &written, portMAX_DELAY);
                total_written += written;
                vTaskDelay(1);
            }
            Serial.printf("[TTS] 写入I2S: %d字节\n", total_written);

            // 等待播放完成
            int wait_ms = (length - offset) / 32 + 200;
            Serial.printf("[TTS] 播放中...等待%d毫秒\n", wait_ms);
            delay(wait_ms);

            i2s_zero_dma_buffer(I2S_PORT_OUT);
            Serial.println("[TTS] 播放完成");

            // 恢复语音识别任务
            if (VoiceTaskHandle != NULL) {
                vTaskResume(VoiceTaskHandle);
                Serial.println("[TTS] 语音识别已恢复");
            }
        } else {
            Serial.printf("[MQTT] 音频长度无效: %d (范围: 1000-%d)\n", length, TTS_AUDIO_BUF_SIZE);
        }
    } else if (strncmp(topic, "blindstick/tts/segments", 23) == 0) {
        // 收到分段音频通知
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, payload, length);
        if (!err) {
            tts_segments_expected = doc["segments"] | 0;
            tts_total_size = doc["totalSize"] | 0;
            tts_segments_received = 0;
            tts_received_size = 0;
            tts_rx_len = 0;
            Serial.printf("[TTS-分段] 准备接收%d段音频，总大小%d字节\n", tts_segments_expected, tts_total_size);
        }
    } else if (strncmp(topic, "blindstick/tts/pcm/", 19) == 0) {
        // 收到音频分段
        int seg_idx = atoi(topic + 19);
        Serial.printf("[TTS-分段] 收到第%d段: %d字节\n", seg_idx, length);

        if (tts_rx_len + length < TTS_AUDIO_BUF_SIZE) {
            memcpy((void*)(tts_rx_buf + tts_rx_len), payload, length);
            tts_rx_len += length;
            tts_segments_received++;
            tts_received_size += length;

            Serial.printf("[TTS-分段] 已接收%d/%d段，累计%d字节\n",
                         tts_segments_received, tts_segments_expected, tts_rx_len);

            // 如果收齐所有段，开始播放
            if (tts_segments_received >= tts_segments_expected && tts_segments_expected > 0) {
                Serial.printf("[TTS-分段] 收齐所有段，开始播放: %d字节\n", tts_rx_len);

                // 暂停语音识别任务，释放内存
                if (VoiceTaskHandle != NULL) {
                    vTaskSuspend(VoiceTaskHandle);
                    webSocket.disconnect();
                    Serial.println("[TTS] 语音识别已暂停");
                }

                // 检查WAV头
                int offset = 0;
                if (tts_rx_len > 44 && tts_rx_buf[0]=='R' && tts_rx_buf[1]=='I' && tts_rx_buf[2]=='F' && tts_rx_buf[3]=='F') {
                    offset = 44;
                    Serial.println("[TTS] 跳过WAV头");
                }

                // 清空I2S缓冲区
                i2s_zero_dma_buffer(I2S_PORT_OUT);

                // 写入I2S（分块写入）
                const int CHUNK_SIZE = 4096;
                int total_written = 0;
                size_t written = 0;

                while (total_written < tts_rx_len - offset) {
                    int to_write = min(CHUNK_SIZE, tts_rx_len - offset - total_written);
                    i2s_write(I2S_PORT_OUT, (const void*)(tts_rx_buf + offset + total_written), to_write, &written, portMAX_DELAY);
                    total_written += written;
                    vTaskDelay(1);
                }

                Serial.printf("[TTS] 写入I2S: %d字节\n", total_written);

                // 等待播放完成
                int wait_ms = (tts_rx_len - offset) / 32 + 200;
                Serial.printf("[TTS] 播放中...等待%d毫秒\n", wait_ms);
                delay(wait_ms);

                i2s_zero_dma_buffer(I2S_PORT_OUT);
                Serial.println("[TTS] 播放完成");

                // 恢复语音识别任务
                if (VoiceTaskHandle != NULL) {
                    vTaskResume(VoiceTaskHandle);
                    Serial.println("[TTS] 语音识别已恢复");
                }

                // 重置
                tts_segments_expected = 0;
                tts_segments_received = 0;
                tts_rx_len = 0;
            }
        } else {
            Serial.println("[TTS-分段] 缓冲区溢出，丢弃");
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
        // 收到TTS请求（网页端发送文本，ESP32自己合成）
        StaticJsonDocument<256> doc;
        DeserializationError err = deserializeJson(doc, payload, length);
        if (!err && doc.containsKey("text")) {
            const char* text = doc["text"];
            Serial.printf("[MQTT-TTS] 收到TTS请求: %s\n", text);
            // ESP32自己调用百度TTS合成并播放
            baiduTTSPlay(text);
        }
    }
}

// 全局变量
bool last_blocked = false;
unsigned long last_alert_time = 0;
float last_alert_dist = 0;
#define ALERT_INTERVAL_MS 3000  // 障碍物告警间隔 3 秒
#define ALERT_DIST_CHANGE 30    // 距离变化超过30cm才重新播报

// ==================== 障碍物语音告警 ====================
void checkObstacleAndAlert() {
    // 获取当前最小距离和方向
    float min_dist = 400.0f;
    const char* direction = "";

    // 找到最小距离的方向（简化为只检测正前、左前、右前三个方向）
    if (dir_smt[0] < min_dist) { min_dist = dir_smt[0]; direction = "前方"; }
    if (dir_smt[1] < min_dist) { min_dist = dir_smt[1]; direction = "右前方"; }
    if (dir_smt[2] < min_dist) { min_dist = dir_smt[2]; direction = "左前方"; }

    unsigned long now = millis();

    // 如果有障碍物且距离小于阈值
    if (min_dist < ALERT_DIST_CM) {
        // 检查是否应该播报
        bool should_alert = false;

        if (!last_blocked) {
            // 首次检测到障碍物
            should_alert = true;
            Serial.printf("[告警] 首次检测: %s%.0fcm\n", direction, min_dist);
        } else if (now - last_alert_time > ALERT_INTERVAL_MS) {
            // 超过间隔时间
            should_alert = true;
            Serial.printf("[告警] 间隔触发: %s%.0fcm (上次%.0fcm)\n",
                         direction, min_dist, last_alert_dist);
        } else if (fabs(min_dist - last_alert_dist) > ALERT_DIST_CHANGE) {
            // 距离变化较大
            should_alert = true;
            Serial.printf("[告警] 距离变化: %.0fcm -> %.0fcm\n", last_alert_dist, min_dist);
        }

        if (should_alert && mqtt.connected()) {
            // 构建告警文本
            char alert_msg[128];
            if (min_dist < 100.0f) {
                snprintf(alert_msg, sizeof(alert_msg), "注意！%s%.0f厘米有障碍物，请立即避让",
                         direction, min_dist);
            } else {
                snprintf(alert_msg, sizeof(alert_msg), "%s%.0f厘米有障碍物",
                         direction, min_dist);
            }

            // 通过 MQTT 发送 TTS 请求
            StaticJsonDocument<256> doc;
            doc["type"] = "obstacle_alert";
            doc["text"] = alert_msg;
            doc["distance"] = min_dist;
            doc["direction"] = direction;

            char buf[256];
            size_t len = serializeJson(doc, buf, sizeof(buf));

            // 使用 QoS 1 确保消息送达
            bool sent = mqtt.publish(MQTT_TOPIC_TTS_REQ, buf, len);

            if (sent) {
                Serial.printf("[告警] 已发送: %s\n", alert_msg);
                last_alert_time = now;
                last_alert_dist = min_dist;
            } else {
                Serial.println("[告警] MQTT发送失败!");
            }
        }
        last_blocked = true;
    } else {
        // 障碍物消失，重置状态
        if (last_blocked) {
            Serial.println("[告警] 障碍物已清除");
        }
        last_blocked = false;
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
    if (ok) Serial.printf("[MQTT] 已发布 %d bytes\n", n);
    else Serial.println("[MQTT] 发布失败");
}

// ==================== 从 MQTT 回调接收 TTS 音频并播放 ====================
void processMqttTtsAudio() {
    if (!tts_rx_ready) return;

    Serial.printf("[TTS处理] 准备处理音频，大小=%d 字节\n", tts_rx_len);

    // 超时检查
    if (millis() - tts_rx_start > TTS_RX_TIMEOUT_MS) {
        Serial.println("[TTS处理] 接收超时，丢弃");
        tts_rx_ready = false;
        return;
    }

    // 根据当前状态决定优先级
    TTS_Priority priority;
    if (is_blocked) {
        priority = TTS_PRIORITY_HIGH;
        audio_request_state = AUDIO_AVOID;
        Serial.println("[TTS处理] 优先级: 高(避障)");
    } else if (nav_active) {
        priority = TTS_PRIORITY_NORMAL;
        audio_request_state = AUDIO_NAV;
        Serial.println("[TTS处理] 优先级: 中(导航)");
    } else {
        priority = TTS_PRIORITY_LOW;
        audio_request_state = AUDIO_CHAT;
        Serial.println("[TTS处理] 优先级: 低(对话)");
    }

    // 分配内存并复制
    uint8_t* buf = (uint8_t*)malloc(tts_rx_len);
    if (!buf) {
        Serial.println("[TTS处理] 内存不足，无法分配");
        tts_rx_ready = false;
        return;
    }

    memcpy(buf, (const void*)tts_rx_buf, tts_rx_len);

    if (play_tts_audio(buf, tts_rx_len, priority)) {
        Serial.printf("[TTS处理] 音频已入队，优先级=%d，大小=%d\n", priority, tts_rx_len);
    } else {
        Serial.println("[TTS处理] 队列满，丢弃音频");
        free(buf);
    }

    tts_rx_ready = false;
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
        if (is_blocked)      audio_request_state = AUDIO_AVOID;
        else if (nav_active) audio_request_state = AUDIO_NAV;
        else                 audio_request_state = AUDIO_IDLE;

        if (WiFi.status() == WL_CONNECTED) {
            // 确保 MQTT 连接成功
            if (!mqtt.connected()) {
                mqtt_reconnect();
            }

        if (mqtt.connected()) {
                mqtt.loop();  // 保活

                // 处理 TTS 音频 - 直接播放，不经过队列
                if (tts_rx_ready) {
                    Serial.printf("[主循环] 直接播放TTS音频: %d字节\n", tts_rx_len);

                    // 直接播放，不入队
                    uint8_t* pcm_data = (uint8_t*)tts_rx_buf;
                    int pcm_len = tts_rx_len;

                    // 检查是否是WAV格式（有44字节头）
                    int offset = 0;
                    if (pcm_len > 44 && pcm_data[0]=='R' && pcm_data[1]=='I' && pcm_data[2]=='F' && pcm_data[3]=='F') {
                        offset = 44;
                        Serial.println("[直接播放] 检测到WAV格式，跳过44字节头");
                    } else {
                        Serial.println("[直接播放] 原始PCM数据，无WAV头");
                    }

                    if (pcm_len > offset) {
                        Serial.printf("[直接播放] 开始播放，数据大小: %d字节\n", pcm_len - offset);

                        // 清空I2S缓冲区
                        i2s_zero_dma_buffer(I2S_PORT_OUT);

                        // 分块写入，避免缓冲区溢出
                        const int CHUNK_SIZE = 4096;
                        int total_written = 0;
                        size_t written = 0;

                        while (total_written < pcm_len - offset) {
                            int to_write = min(CHUNK_SIZE, pcm_len - offset - total_written);
                            i2s_write(I2S_PORT_OUT, pcm_data + offset + total_written, to_write, &written, portMAX_DELAY);
                            total_written += written;
                            vTaskDelay(1); // 给系统一点时间
                        }

                        Serial.printf("[直接播放] 已写入I2S: %d字节\n", total_written);

                        // 等待播放完成（根据数据量计算时间）
                        // 16000Hz, 16bit, 单声道 = 32000字节/秒
                        int play_time_ms = (pcm_len - offset) / 32;
                        Serial.printf("[直接播放] 等待%d毫秒播放完成...\n", play_time_ms);
                        delay(play_time_ms + 100);

                        i2s_zero_dma_buffer(I2S_PORT_OUT);
                        Serial.println("[直接播放] 播放完成");
                    }

                    tts_rx_ready = false;
                    tts_rx_len = 0;
                }

                // 障碍物检测和语音告警（使用百度TTS）
                checkObstacleAndAlertBaidu();

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
                if (audio_request_state == AUDIO_AVOID || is_ai_talking) {
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
        .sample_rate = 16000, .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8, .dma_buf_len = 1024,
        .use_apll = false, .tx_desc_auto_clear = false, .fixed_mclk = 0
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_SCK_PIN, .ws_io_num = I2S_WS_PIN,
        .data_out_num = I2S_PIN_NO_CHANGE, .data_in_num = I2S_SD_PIN
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
    err = i2s_set_clk(I2S_PORT, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
    if (err != ESP_OK) {
        Serial.printf("[I2S] 时钟设置失败: %d\n", err);
        return;
    }
    Serial.println("[I2S] 麦克风初始化成功");
}

void i2s_out_init() {
    Serial.printf("[I2S-OUT] 扬声器引脚配置: BCK=%d, WS=%d, DATA=%d\n", I2S_BCK_PIN, I2S_WS_OUT_PIN, I2S_DATA_PIN);

    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = 16000, .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
        .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
        .communication_format = I2S_COMM_FORMAT_STAND_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 8, .dma_buf_len = 1024,
        .use_apll = true, .tx_desc_auto_clear = true, .fixed_mclk = 0
    };
    i2s_pin_config_t pin_config = {
        .bck_io_num = I2S_BCK_PIN, .ws_io_num = I2S_WS_OUT_PIN,
        .data_out_num = I2S_DATA_PIN, .data_in_num = I2S_PIN_NO_CHANGE
    };
    esp_err_t err = i2s_driver_install(I2S_PORT_OUT, &i2s_config, 0, NULL);
    if (err != ESP_OK) { Serial.printf("[I2S-OUT] 驱动安装失败: %d\n", err); return; }
    err = i2s_set_pin(I2S_PORT_OUT, &pin_config);
    if (err != ESP_OK) { Serial.printf("[I2S-OUT] 引脚设置失败: %d\n", err); return; }

    // 设置时钟 - 关键：使用 APLL 获得精确 16kHz，确保 TTS 播放不失真
    err = i2s_set_clk(I2S_PORT_OUT, 16000, I2S_BITS_PER_SAMPLE_16BIT, I2S_CHANNEL_MONO);
    if (err != ESP_OK) {
        Serial.printf("[I2S-OUT] 时钟设置失败: %d\n", err);
        return;
    }

    Serial.println("[I2S-OUT] MAX98357功放初始化成功 (APLL 时钟)");
}

// ==================== 播放启动测试语音 ====================
void playStartupTestTone() {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    String url = String("http://") + "192.168.3.24" + ":" + "8080" + "/api/tts";
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(10000);
    String jsonPayload = "{\"text\":\"启动成功\"}";
    int httpCode = http.POST(jsonPayload);
    if (httpCode == 200) {
        String contentType = http.header("Content-Type");
        if (contentType.indexOf("audio/wav") >= 0 || contentType.indexOf("audio/x-wav") >= 0) {
            int len = http.getSize();
            Serial.printf("[启动播报] 音频 %d 字节\n", len);
            WiFiClient* stream = http.getStreamPtr();
            uint8_t header[44]; stream->readBytes(header, 44);
            const int CHUNK_BUF = 4096;
            uint8_t* buf = (uint8_t*)malloc(len);
            if (!buf) { http.end(); Serial.println("[启动播报] 内存不足"); return; }
            memcpy(buf, header, 44);
            int totalRead = 44;
            uint8_t* chunk = (uint8_t*)malloc(CHUNK_BUF);
            if (chunk) {
                while (totalRead < len) {
                    int toRead = min(CHUNK_BUF, len - totalRead);
                    int r = stream->readBytes(chunk, toRead);
                    if (r <= 0) break;
                    memcpy(buf + totalRead, chunk, r); totalRead += r;
                }
                free(chunk);
            }
            // play_tts_audio 成功后，buf 所有权转移给 TTS 队列，不得 free
            if (play_tts_audio(buf, totalRead, TTS_PRIORITY_HIGH)) {
                Serial.printf("[启动播报] 入队 %d 字节\n", totalRead);
            } else {
                free(buf);
            }
            http.end(); return;
        }
    }
    Serial.printf("[启动播报] 失败 %d\n", httpCode);
    http.end();
}

// 本地生成启动提示音 - 简化为单次蜂鸣，减少内存占用
void playLocalStartupTone() {
    Serial.println("[启动提示] 播放测试音...");

    const int sample_rate = 16000;
    const int num_samples = sample_rate / 2; // 0.5秒

    // 使用静态缓冲区，避免堆分配
    static int16_t tone_buffer[8000]; // 0.5秒 @ 16kHz

    // 生成蜂鸣音
    for (int i = 0; i < num_samples; i++) {
        float t = (float)i / sample_rate;
        // 1kHz正弦波
        float sample = sin(2 * PI * 1000 * t) * 8000.0f;
        tone_buffer[i] = (int16_t)sample;
    }

    // 播放
    size_t written = 0;
    i2s_zero_dma_buffer(I2S_PORT_OUT);
    i2s_write(I2S_PORT_OUT, tone_buffer, num_samples * 2, &written, portMAX_DELAY);

    Serial.printf("[启动提示] 播放 %d 字节\n", written);
    delay(600);
    i2s_zero_dma_buffer(I2S_PORT_OUT);
}

void play_wav_audio(uint8_t* wav_data, int wav_len) {
    if (!wav_data || wav_len < 100) return;
    if (play_tts_audio(wav_data, wav_len, TTS_PRIORITY_NORMAL))
        Serial.println("[播放] 音频已入队");
    else Serial.println("[播放] 入队失败");
}

size_t record_audio(uint8_t* buffer, size_t buffer_size) {
    size_t bytes_read = 0, total_read = 0;
    int valid_samples = 0, clipped_samples = 0, total_samples = 0;
    int32_t avg_amplitude = 0;
    memset(buffer, 0, buffer_size);
    unsigned long start_time = millis();
    Serial.println("[录音] 开始...");
    while (total_read < buffer_size && (millis() - start_time) < 2500) {
        esp_err_t err = i2s_read(I2S_PORT, buffer + total_read, buffer_size - total_read,
                                &bytes_read, 100 / portTICK_PERIOD_MS);
        if (err == ESP_OK && bytes_read > 0) {
            int16_t* samples = (int16_t*)(buffer + total_read);
            int num_samples = bytes_read / 2;
            for (int i = 0; i < num_samples; i++) {
                int16_t sample = samples[i];
                if (sample > 30000 || sample < -30000) clipped_samples++;
                if (sample != 0 && sample != -1 && abs(sample) > 100) {
                    valid_samples++; avg_amplitude += abs(sample);
                }
                total_samples++;
            }
            total_read += bytes_read;
        }
        static unsigned long last_print = 0;
        if (millis() - last_print > 500) {
            Serial.printf("[录音] %d字节 有效:%d 削顶:%d\n", total_read, valid_samples, clipped_samples);
            last_print = millis();
        }
    }
    if (valid_samples > 0) avg_amplitude /= valid_samples;
    float clip_ratio = (float)clipped_samples / total_samples;
    if (valid_samples < 100) Serial.println("[录音] 警告：音频几乎全为零");
    else if (clip_ratio > 0.3) Serial.printf("[录音] 削顶%.1f%%\n", clip_ratio * 100);
    else Serial.printf("[录音] 完成 %d字节 削顶%.1f%%\n", total_read, clip_ratio * 100);
    return total_read;
}

void fetchNavStepsFromServer() {
    if (WiFi.status() != WL_CONNECTED) return;
    HTTPClient http;
    String url = String("http://") + "192.168.3.24" + ":" + "8080" + "/api/nav_steps";
    http.begin(url);
    int httpCode = http.GET();
    if (httpCode == 200) {
        String response = http.getString();
        StaticJsonDocument<4096> doc;
        DeserializationError error = deserializeJson(doc, response);
        if (!error && doc["status"] == "ok") {
            JsonArray steps = doc["steps"];
            nav_total_steps = 0;
            for (const char* step : steps) {
                nav_steps[nav_total_steps] = String(step);
                nav_total_steps++; if (nav_total_steps >= 10) break;
            }
            current_step_idx = 0; current_progress = 0; nav_active = true;
            Serial.printf("[导航] 获取路线 %d 步\n", nav_total_steps);
        }
    }
    http.end();
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

    // 录音2秒 (64KB)
    const int RECORD_SIZE = 16000 * 2 * 2;
    uint8_t* buffer = (uint8_t*)malloc(RECORD_SIZE);
    if (!buffer) {
        Serial.println("[ASR-REST] 内存不足");
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

    // 获取Token
    String token = getBaiduAccessToken();
    if (token.length() == 0) {
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
        Serial.println("[诊断] 请检查:");
        Serial.println("       1. INMP441 VDD 是否接 3.3V (不能接5V!)");
        Serial.println("       2. GND 是否正确连接");
        Serial.println("       3. 引脚接线:");
        Serial.println("          WS  → GPIO 2");
        Serial.println("          SCK → GPIO 1");
        Serial.println("          SD  → GPIO 42");
        Serial.println("       4. L/R 引脚是否接地 (接GND=左声道)");
        Serial.println("       5. INMP441 模块是否损坏 (换一个好的试试)");
    } else {
        Serial.println("[诊断] ✅ 麦克风硬件正常");
    }
    Serial.println();

    // 启动 TTS 播放器任务（先启动，等联网后再播报）
    xTaskCreatePinnedToCore(TTSPlayerTask, "TTSPlayer", 8192, NULL, 4, &TTSPlayerTaskHandle, 1);
    Serial.println("[系统] TTS播放任务已启动");
    delay(100);

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
        // 初始化 MQTT
        espClient.setInsecure();  // 开发阶段跳过证书校验
        mqtt.setServer(MQTT_BROKER, MQTT_PORT);
        mqtt.setCallback(mqtt_callback);

        // 连接MQTT（非阻塞重试）
        mqtt_reconnect();

        // WiFi 已连接，使用MQTT代理播报启动成功
        delay(500);
        requestTTSViaMQTT("导盲杖系统启动成功，欢迎使用");
        Serial.println("[系统] 启动播报请求已发送");
    } else {
        Serial.println("[WiFi] 离线模式，MQTT不可用，跳过启动播报");
    }

    Serial.println("[系统] 等待语音输入目的地...");

    xTaskCreatePinnedToCore(RadarMotorUploadTask, "RadarTask", 4096, NULL, 3, &RadarTaskHandle, 0);
    xTaskCreatePinnedToCore(NavigationTask, "NavTask", 2048, NULL, 1, &NavTaskHandle, 1);
    xTaskCreatePinnedToCore(VoiceRecognitionTask, "VoiceRecTask", 4096, NULL, 2, &VoiceTaskHandle, 1);

    delay(300);
}

void loop() {
    vTaskDelete(NULL);
}

// ==================== 新增：百度语音 API 功能 ====================

/**
 * URL编码辅助函数
 */
String urlEncode(const char* str) {
    String encoded = "";
    char c;
    for (int i = 0; str[i] != '\0'; i++) {
        c = str[i];
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += "%20";
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
 * 通过MQTT请求TTS（当直接HTTPS失败时使用）
 */
bool requestTTSViaMQTT(const char* text) {
    if (!mqtt.connected()) {
        Serial.println("[TTS-MQTT] MQTT未连接");
        return false;
    }

    StaticJsonDocument<256> doc;
    doc["type"] = "tts_request";
    doc["text"] = text;
    doc["device"] = "esp32";

    char buf[256];
    size_t len = serializeJson(doc, buf, sizeof(buf));

    bool sent = mqtt.publish("blindstick/tts/request", buf, len);
    if (sent) {
        Serial.printf("[TTS-MQTT] 已发送TTS请求: %s\n", text);
    } else {
        Serial.println("[TTS-MQTT] 发送失败");
    }
    return sent;
}

/**
 * 获取百度 Access Token（带缓存，避免频繁请求）
 */
String getBaiduAccessToken() {
    // 检查缓存的Token是否有效（提前5分钟过期）
    if (baidu_access_token.length() > 0 && millis() < baidu_token_expire - 300000) {
        Serial.println("[百度Token] 使用缓存的Token");
        return baidu_access_token;
    }

    // 如果刚刚失败过，等待一段时间再重试（避免频繁请求导致崩溃）
    static unsigned long lastRetryTime = 0;
    static int retryCount = 0;
    if (millis() - lastRetryTime < 10000) { // 10秒内不重试
        Serial.println("[百度Token] 请求过于频繁，使用缓存或等待");
        return baidu_access_token; // 返回旧的，即使可能过期
    }
    lastRetryTime = millis();
    retryCount++;

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[百度Token] WiFi未连接");
        return "";
    }

    Serial.println("[百度Token] 正在请求Access Token...");

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10000); // 减少超时时间

    HTTPClient http;
    String url = String(BAIDU_TOKEN_URL) +
                 "?grant_type=client_credentials" +
                 "&client_id=" + BAIDU_API_KEY.c_str() +
                 "&client_secret=" + BAIDU_SECRET_KEY.c_str();

    if (!http.begin(client, url)) {
        Serial.println("[百度Token] HTTP初始化失败");
        return "";
    }

    http.setTimeout(10000);
    http.setReuse(false);

    int httpCode = http.GET();
    Serial.printf("[百度Token] HTTP返回码: %d\n", httpCode);

    if (httpCode == 200) {
        String response = http.getString();
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, response);
        if (!error && doc.containsKey("access_token")) {
            baidu_access_token = doc["access_token"].as<String>();
            int expires_in = doc["expires_in"] | 2592000;
            baidu_token_expire = millis() + expires_in * 1000;
            retryCount = 0; // 重置重试计数
            Serial.printf("[百度Token] 获取成功, 有效期%d秒\n", expires_in);
            http.end();
            return baidu_access_token;
        }
    }

    Serial.printf("[百度Token] 获取失败: %d, 重试次数: %d\n", httpCode, retryCount);
    http.end();
    return ""; // 返回空，让调用方使用MQTT代理
}

/**
 * 百度TTS语音合成并直接播放（POST方式，与官方示例一致）
 * @param text 要合成的文本
 * @return 是否成功
 */
bool baiduTTSPlay(const char* text) {
    String token = getBaiduAccessToken();
    if (token.length() == 0) {
        Serial.println("[百度TTS] 无法获取Token，尝试MQTT代理...");
        // 直接连接失败，通过MQTT让网页端代为合成
        return requestTTSViaMQTT(text);
    }

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[百度TTS] WiFi未连接");
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;

    Serial.printf("[百度TTS] 合成文本: %s\n", text);

    if (!http.begin(client, BAIDU_TTS_URL.c_str())) {
        Serial.println("[百度TTS] HTTP初始化失败");
        return false;
    }

    http.setTimeout(15000);
    http.setReuse(false);

    // 构造POST参数（使用基础音库，避免权限问题）
    String postData = "tex=" + urlEncode(text) +
                      "&tok=" + token +
                      "&cuid=esp32_blindstick" +
                      "&ctp=1" +
                      "&lan=zh" +
                      "&spd=5" +
                      "&pit=5" +
                      "&vol=9" +
                      "&per=0" +     // 基础音库：度小美
                      "&aue=6";      // wav格式

    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.addHeader("Accept", "*/*");

    Serial.println("[百度TTS] 发送POST请求...");
    int httpCode = http.POST(postData);
    Serial.printf("[百度TTS] HTTP返回码: %d\n", httpCode);

    if (httpCode == 200) {
        String contentType = http.header("Content-Type");
        Serial.printf("[百度TTS] Content-Type: %s\n", contentType.c_str());

        if (contentType.indexOf("audio") >= 0) {
            int len = http.getSize();
            Serial.printf("[百度TTS] 收到音频: %d字节\n", len);

            WiFiClient* stream = http.getStreamPtr();

            // 读取音频数据
            uint8_t* audioBuf = (uint8_t*)malloc(len);
            if (!audioBuf) {
                Serial.println("[百度TTS] 内存不足");
                http.end();
                return false;
            }

            int totalRead = 0;
            while (totalRead < len) {
                int available = stream->available();
                if (available > 0) {
                    int toRead = min(available, len - totalRead);
                    int r = stream->readBytes(audioBuf + totalRead, toRead);
                    if (r > 0) totalRead += r;
                }
                if (totalRead >= len) break;
                delay(1);
            }

            http.end();

            Serial.printf("[百度TTS] 读取音频数据: %d字节\n", totalRead);

            // 播放音频（入队）
            bool ok = play_tts_audio(audioBuf, totalRead, TTS_PRIORITY_HIGH);
            if (ok) {
                Serial.println("[百度TTS] 已入队播放");
                return true;
            } else {
                free(audioBuf);
            }
        } else {
            // 收到错误信息
            String error = http.getString();
            Serial.printf("[百度TTS] 错误: %s\n", error.c_str());
        }
    } else {
        Serial.printf("[百度TTS] HTTP错误: %d, %s\n", httpCode, http.errorToString(httpCode).c_str());
    }

    http.end();

    // 直接HTTPS失败，尝试通过MQTT
    Serial.println("[百度TTS] 直接连接失败，尝试通过MQTT...");
    return requestTTSViaMQTT(text);
}

/**
 * 播放开机启动成功语音
 */
void playStartupSuccess() {
    Serial.println("[开机播报] 播放启动成功...");
    baiduTTSPlay("导盲杖系统启动成功，欢迎使用");
}

/**
 * 播报障碍物告警（带距离）
 */
void announceObstacle(float distance, const char* direction) {
    char alertText[128];

    if (distance < 50) {
        snprintf(alertText, sizeof(alertText), "%s%.0f厘米有障碍物，请立即避让", direction, distance);
    } else if (distance < 100) {
        snprintf(alertText, sizeof(alertText), "%s%.0f厘米有障碍物，请注意避让", direction, distance);
    } else if (distance < 200) {
        snprintf(alertText, sizeof(alertText), "%s%.0f厘米有障碍物", direction, distance);
    } else {
        snprintf(alertText, sizeof(alertText), "%s%.0f米有障碍物", direction, distance / 100);
    }

    Serial.printf("[障碍物播报] %s\n", alertText);
    baiduTTSPlay(alertText);
}

// ==================== 新增：语音识别和导航功能 ====================

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

    // 播报导航开始
    char navText[256];
    snprintf(navText, sizeof(navText), "开始导航到%s，全程%d米，预计%d分钟，%s",
             destName.c_str(), distance, duration / 60, nav_steps[0].c_str());
    baiduTTSPlay(navText);

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
        baiduTTSPlay("抱歉，没有找到该地点");
        return;
    }

    // 检查距离是否太远
    if (distance > MAX_NAVIGATION_DISTANCE) {
        char msg[128];
        snprintf(msg, sizeof(msg), "目的地%s距离您%.1f公里，距离太远，请重新选择较近的地点",
                 destName.c_str(), distance / 1000);
        baiduTTSPlay(msg);
        return;
    }

    // 规划路线
    if (planWalkingRoute(destLat, destLng, destName)) {
        Serial.printf("[导航] 已开始导航到: %s\n", destName.c_str());
    } else {
        baiduTTSPlay("路线规划失败，请重试");
    }
}

/**
 * 改进的障碍物检测和播报（使用百度TTS）
 */
void checkObstacleAndAlertBaidu() {
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
            announceObstacle(min_dist, direction);
            last_alert_time = now;
            last_alert_dist = min_dist;
        }
        last_blocked = true;
    } else {
        last_blocked = false;
    }
}
