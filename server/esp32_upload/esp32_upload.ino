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
#include <esp_system.h>  // 【诊断】esp_reset_reason：区分重启原因是掉电/panic/看门狗
#include <Preferences.h>  // 【修复】NVS持久化：常住地设置重启/烧录后仍保留
// ==================== 网络参数 ====================
const char* WIFI_SSID     = "ZYT";
const char* WIFI_PASSWORD = "zyt123456";
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
// 【修复】MQTT音频：Render分片推送(blindstick/tts/stream/N)，
//        固件重装成完整PCM后入队，由独立任务播放。
//        绕开被热点/运营商挡住的Render 443下载（URL路径getSize=-1无声）。
#define TTS_MQTT_CHUNK_MAX  4000          // 每分片字节，须小于 mqtt.setBufferSize(8192)
#define TTS_MQTT_BUF_MAX    (210 * 1024)  // 整句PCM上限210KB（约6.5秒@16kHz；高于代理200KB放行上限，防整句超限丢弃）
uint8_t* tts_pcm_buf = NULL;              // 分片重装缓冲（PSRAM优先）
volatile size_t tts_pcm_len = 0;
volatile bool tts_pcm_assembling = false; // stream_start 后为 true，接收分片
typedef struct {
    uint8_t* data;
    size_t len;
} TTSPcmMsg;
QueueHandle_t ttsPcmQueue = NULL;
// TTS音频缓冲区（使用PSRAM动态分配，不占用主内存）
#define TTS_AUDIO_BUF_SIZE  (80 * 1024)  // 减小到80KB，足够播放
// ==================== 百度语音 API 配置（仅用于语音识别）====================
const String BAIDU_API_KEY        = "Xbxnhkwb2sxtB6HbH5BUTlUG";
const String BAIDU_SECRET_KEY     = "Tw485P2BFGpPu8WeOVP6hy4S1BHqG4ON";
// ==================== 语音识别和导航配置 ====================
#define MAX_NAVIGATION_DISTANCE 10000  // 最大导航距离10公里（米）
// 导航触发词
const char* NAV_TRIGGERS[] = {"带我去", "我要去", "我想去", "导航到", "我去", "去", "到"};
const int NAV_TRIGGER_COUNT = 7;
// 需要过滤的非目的地词汇
// 【修复】删掉单字过滤项(在/是/我/有/和/就/不/人/都/一/请/把/给/跟/对/向/从/让/被/比)：
// 它们常是地名的一部分，replace()会拆坏真实地名：人民医院→民医院、成都→成、人民广场→民广场。
// 触发词提取(带我去/我要去/...)已吃掉"请带我去"这类前缀，这里只需清"附近/那个"等尾巴。
const char* FILTER_WORDS[] = {
"的", "了", "一个", "一下", "那个", "这个", "那里", "这里", "吧", "啊", "呢", "吗", "哦", "嗯",
"附近", "周围", "旁边", "对面"
};
const int FILTER_WORD_COUNT = 18;
// ==================== 雷达与电机引脚 ====================
#define RADAR_RX_PIN    18    // YDLIDAR X2 TX → ESP32 GPIO18 (UART RX)
#define RADAR_M_CTR_PIN 8     // YDLIDAR X2 电机控制 → ESP32 GPIO8（启动雷达电机）
#define MOTOR_IN1       12    // TB6612 AIN1 → GPIO12
#define MOTOR_IN2       11    // TB6612 AIN2 → GPIO11
#define MOTOR_PWM       10    // TB6612 PWMA → GPIO10
#define MOTOR_STBY      9     // 【新增】TB6612 STBY → GPIO9（必须置高电机才能工作）
// ==================== YDLIDAR X2 启动命令 ====================
static const uint8_t YDLIDAR_CMD_START[] = { 0xA5, 0x60, 0x00, 0x60, 0x01, 0x00, 0x60, 0xE8 };
static const uint8_t YDLIDAR_CMD_STOP[]  = { 0xA5, 0x65, 0x00, 0x65, 0x01, 0x00, 0x65, 0x1B };
static const uint8_t YDLIDAR_CMD_RESET[] = { 0xA5, 0x40, 0x00, 0x40, 0x01, 0x00, 0x40, 0x97 };
// ==================== GPS软串口配置 ====================
// GPS改为软串口（原K230引脚）
// 【修复】RX/TX接反：ATGM336H TX → GPIO17(软串口RX)，GPS RX → GPIO16(软串口TX)
#define GPS_SOFT_RX_PIN     17   // GPS TX → ESP32 GPIO17 (软串口RX)
#define GPS_SOFT_TX_PIN     16   // GPS RX → ESP32 GPIO16 (软串口TX)
SoftwareSerial gpsSerial(GPS_SOFT_RX_PIN, GPS_SOFT_TX_PIN);  // GPS软串口
// ==================== K230硬件串口配置 ====================
// K230使用硬件串口UART2
#define K230_UART_ID        2      // UART2
#define K230_UART_BAUD      115200
#define K230_RX_PIN         15      // K230 TX → ESP32 GPIO15 (UART2 RX)
#define K230_TX_PIN         7       // K230 RX → ESP32 GPIO7 (UART2 TX)
HardwareSerial k230Serial(2);      // K230硬件串口使用UART2
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
#define VOLUME_GAIN     1.0  // 音量增益 (0.0-1.0)，增大音量（播报更响，1.0=满增益，有削波保护）
// ==================== MQTT 并发保护（防挂死根因修复） ====================
// 【修复】RadarTask(mqtt.loop+发布)、NavTask(发布)、VoiceTask(ASR分片发布) 三个任务在双核上
// 并发操作同一个 PubSubClient/mbedTLS TLS 会话，会踩坏内部状态，最终 mqtt.loop() 永久卡死。
// 现象：轮子停在最后方向一直转、K230 数据不再处理、串口不再更新，只能断电重启。
// 解法：用【递归】互斥锁把所有 mqtt.* 访问串行化。为什么递归：mqtt_callback 在 mqtt.loop()
// 内部执行，回调里再 publish 属于重入，普通互斥锁会自己锁死自己。
bool mqtt_reconnect_nonblocking();  // 前向声明
SemaphoreHandle_t mqttMutex = NULL;
// 【修复】发布锁超时分级：
//  - 普通发布 500ms：VoiceTask ASR分片/NavTask 播报等非避障关键路径，短握手中断可接受
//  - RadarTask 内 K230 播报走 mqttPublishShort：50ms，绝不让避障循环等网络锁
// 统一原则：任何发布都不永久等锁——MQTTTask 握手可能持锁≤3秒，永久等待会让
// 调用方任务被网络锁卡住（此前 RadarTask 因此避障停顿/心跳停滞→看门狗误触发）。
#define MQTT_PUB_LOCK_TIMEOUT_MS  500
bool mqttPublish(const char* topic, const char* payload) {
    if (mqttMutex == NULL) return false;
    if (xSemaphoreTakeRecursive(mqttMutex, MQTT_PUB_LOCK_TIMEOUT_MS / portTICK_PERIOD_MS) != pdTRUE) return false;
    bool ok = mqtt.publish(topic, payload);
    xSemaphoreGiveRecursive(mqttMutex);
    return ok;
}
bool mqttPublish(const char* topic, const char* payload, size_t len) {
    if (mqttMutex == NULL) return false;
    if (xSemaphoreTakeRecursive(mqttMutex, MQTT_PUB_LOCK_TIMEOUT_MS / portTICK_PERIOD_MS) != pdTRUE) return false;
    // 显式转 uint8_t*：PubSubClient 3参重载是 (topic,uint8_t*,plength)，
    // 若传 const char* 会误绑到 (topic,const char*,bool retained)，把长度当 retained 标志
    bool ok = mqtt.publish(topic, (const uint8_t*)payload, len);
    xSemaphoreGiveRecursive(mqttMutex);
    return ok;
}
bool mqttPublish(const char* topic, const uint8_t* payload, size_t len) {
    if (mqttMutex == NULL) return false;
    if (xSemaphoreTakeRecursive(mqttMutex, MQTT_PUB_LOCK_TIMEOUT_MS / portTICK_PERIOD_MS) != pdTRUE) return false;
    bool ok = mqtt.publish(topic, payload, len);
    xSemaphoreGiveRecursive(mqttMutex);
    return ok;
}
bool mqttPublish(const char* topic, const uint8_t* payload, size_t len, bool retained) {
    if (mqttMutex == NULL) return false;
    if (xSemaphoreTakeRecursive(mqttMutex, MQTT_PUB_LOCK_TIMEOUT_MS / portTICK_PERIOD_MS) != pdTRUE) return false;
    bool ok = mqtt.publish(topic, payload, len, retained);
    xSemaphoreGiveRecursive(mqttMutex);
    return ok;
}
// 【避障安全】RadarTask 内专用短超时发布：K230播报/传感器不因网络锁卡避障
bool mqttPublishShort(const char* topic, const char* payload, size_t len) {
    if (mqttMutex == NULL) return false;
    if (xSemaphoreTakeRecursive(mqttMutex, 50 / portTICK_PERIOD_MS) != pdTRUE) return false;
    bool ok = mqtt.publish(topic, (const uint8_t*)payload, len);
    xSemaphoreGiveRecursive(mqttMutex);
    return ok;
}
void mqttLoop() {
    if (mqttMutex == NULL) return;
    xSemaphoreTakeRecursive(mqttMutex, portMAX_DELAY);
    mqtt.loop();
    xSemaphoreGiveRecursive(mqttMutex);
}
void mqttReconnect() {
    if (mqttMutex == NULL) return;
    xSemaphoreTakeRecursive(mqttMutex, portMAX_DELAY);
    mqtt_reconnect_nonblocking();
    xSemaphoreGiveRecursive(mqttMutex);
}
// =====================================================================
// 【安全】RadarTask 每轮循环喂一次心跳；SafetyWatchdogTask 据此检测避障主任务是否卡死
volatile unsigned long radarHeartbeat = 0;
// 播放开机语音（云端TTS）
void playStartupVoice() {
Serial.println("[开机语音] 请求云端TTS播报...");
if (mqtt.connected()) {
StaticJsonDocument<256> ttsDoc;
char buf[256];
ttsDoc["text"] = "系统启动成功，欢迎使用智能导盲杖";
ttsDoc["priority"] = PRIO_NORMAL;
size_t len = serializeJson(ttsDoc, buf, sizeof(buf));
mqttPublish(MQTT_TOPIC_TTS_REQ, buf, len);
Serial.println("[开机TTS] 系统启动成功，欢迎使用智能导盲杖");
}
}
// ==================== 音频采样参数 ====================
#define SAMPLE_RATE     16000  // 16kHz 采样率
// ==================== 函数声明 ====================
String urlEncode(const char* str);
float calcDistance(float lat1, float lng1, float lat2, float lng2);
// 流式TTS相关函数
void initStreamingTTS();
void handleStreamControl(const char* payload, int length);
void appendStreamChunk(byte* payload, unsigned int length);
void TTSPcmPlayerTask(void* pvParameters);
void purgeTTSQueue();
// ==================== 流式TTS（MQTT分片 → PCM重装 → 独立任务播放）====================
void playPcmData(uint8_t* data, int len);
void stopCurrentPlayback();
const char* getPrioName(int p);
// 流式语音识别相关
void VoiceRecognitionTask(void* pvParameters);
String doVoiceRecognition();
String getBaiduToken();  // 百度access_token获取（带缓存）
void handleVoiceCommand(const char* text);
String base64Encode(const uint8_t* data, size_t len);  // Base64编码
size_t base64ToBuffer(const uint8_t* data, size_t len, char* out);  // Base64写缓冲区（省内部堆）
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
// 【修复】删除重复定义，只保留一组
#define ANG_FRONT_MIN  330
#define ANG_FRONT_MAX  30
#define ANG_LEFT_MIN   60
#define ANG_LEFT_MAX   120
#define ANG_RIGHT_MIN  240
#define ANG_RIGHT_MAX  300
// 【加快】MQTT上传间隔从200ms改为100ms（10Hz刷新率，平衡实时性和稳定性）
#define UPLOAD_INTERVAL_MS  100
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
String current_destination = "";  // 【新增】当前导航目的地，用于MQTT上报
volatile bool  is_blocked  = false;
volatile bool  is_ai_talking = false;
// 【新增】最近一次实际播放的TTS文本（回声抑制用）
char lastPlayedTtsText[128] = "";
volatile unsigned long ai_talking_start_time = 0;  // 【新增】记录开始时间
const unsigned long AI_TALKING_TIMEOUT_MS = 15000;  // 【新增】15秒超时
volatile unsigned long ttsPlayEndTime = 0;  // 【新增】TTS播放结束时间
const unsigned long TTS_PLAY_SILENT_MS = 5000;  // 【新增】播放后5秒静默期，给语音识别完整录音窗口
const unsigned long TTS_TAIL_GUARD_MS = 1500;  // 【新增】TTS播完后喇叭余音保护期，期间麦克风跳过录音
                                             //        喇叭就在手杖上贴着麦克风，播完的余音+残留DMA音频会被VAD当成说话
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
int last_motor_pwm = 0;  // 【预留】可扩展用于电机状态记录
String last_motor_dir = "stop";  // 【预留】可扩展用于方向记录
float gps_lat = 0.0;
float gps_lng = 0.0;
float gps_speed = 0.0;
int   gps_heading = 0;
int   gps_satellites = 0;
volatile unsigned long gps_byte_count = 0;   // GPS软串口累计收到字节数（诊断用）
// GPS 波特率自动检测（软串口可能收不到默认波特率的数据）
// 【修复】GPS波特率表从9600开始（大多数GPS模块默认9600，避免4秒检测延迟）
const int gps_baud_table[] = {9600, 115200, 38400, 4800};
const int gps_baud_count = 4;
int   gps_baud_index = 0;        // 当前尝试的波特率索引
bool  gps_baud_locked = false;   // 是否已锁定正确的波特率
bool  gps_got_nmea = false;      // 是否收到过有效NMEA（$开头）
unsigned long gps_baud_try_start = 0;  // 当前波特率尝试开始时间
// 常住地设置（默认黄石市，可通过MQTT更新）
// 【修复】默认值从NVS读取（启动时覆盖），收到MQTT设置时写入NVS，重启/烧录后仍保留
String home_city = "黄石市";
Preferences homeCityPrefs;
// 开机语音播报标志（只播报一次）- 使用RTC内存保持，深度睡眠后也能记住
RTC_DATA_ATTR static bool startup_announced_rtc = false;
volatile bool startup_announced = false;  // 运行时标志，用于防止同一运行周期内重复
// ==================== 今日出行统计数据（RTC内存持久化）====================
// 使用RTC内存保持统计数据，即使深度睡眠后也能保留
RTC_DATA_ATTR static uint32_t rtc_total_mileage = 0;      // 总里程（米）
RTC_DATA_ATTR static uint16_t rtc_nav_count = 0;          // 导航次数
RTC_DATA_ATTR static uint16_t rtc_obstacle_count = 0;     // 障碍物提醒次数
RTC_DATA_ATTR static uint16_t rtc_detour_count = 0;       // 路线调整次数
RTC_DATA_ATTR static uint32_t rtc_last_gps_lat = 0;       // 上次GPS纬度（用于计算里程）
RTC_DATA_ATTR static uint32_t rtc_last_gps_lng = 0;       // 上次GPS经度
RTC_DATA_ATTR static bool rtc_has_last_pos = false;       // 是否有上次位置
// 运行时统计变量
float total_mileage = 0.0;        // 总里程（米）
uint16_t nav_count = 0;           // 导航次数
uint16_t obstacle_count = 0;      // 障碍物提醒次数
uint16_t detour_count = 0;        // 路线调整次数
float last_gps_lat_for_mileage = 0.0;  // 上次GPS纬度（用于计算里程）
float last_gps_lng_for_mileage = 0.0;  // 上次GPS经度
bool has_last_gps_pos = false;    // 是否有上次GPS位置
enum LidarState { WAIT_HEADER_AA, WAIT_HEADER_55, READ_CT, READ_LSN, READ_PAYLOAD };
volatile LidarState lidar_state = WAIT_HEADER_AA;
volatile uint8_t packet_ct = 0, packet_lsn = 0;
volatile uint8_t payload_buf[128];
volatile uint8_t payload_idx = 0, payload_expected = 0;
TaskHandle_t RadarTaskHandle = NULL;
TaskHandle_t NavTaskHandle = NULL;
TaskHandle_t VoiceTaskHandle = NULL;
// 【删除】TTSPlayerTaskHandle未使用
// TaskHandle_t TTSPlayerTaskHandle = NULL;
// ==================== K230视觉检测配置 ====================
// 需要播报的目标列表
const char* K230_ALERT_TARGETS[] = {
"red_light",    // 红灯
"green_light",  // 绿灯
"yellow_light", // 黄灯
"stairs",       // 台阶
"person",       // 人
"ashcan",       // 垃圾桶
"curb"          // 路缘石
};
const int K230_ALERT_TARGET_COUNT = 7;
// 5秒防重复播报机制
unsigned long k230_lastAlertTime[7] = {0};
const unsigned long K230_ALERT_COOLDOWN_MS = 8000;
// K230串口接收缓冲区
String k230_receiveBuffer = "";
// K230最新检测数据（用于上传到大屏）
struct K230_Detection {
String targetClass;     // 目标类别英文名
String targetLabel;     // 目标类别中文名
int x, y, w, h;         // 边界框坐标
float confidence;       // 置信度
unsigned long timestamp; // 检测时间戳
};
#define MAX_K230_DETECTIONS 5
K230_Detection k230_detections[MAX_K230_DETECTIONS];
int k230_detection_count = 0;
// ==================== 五向雷达 + EMA平滑 ====================
#define NUM_DIR     3
#define SMOOTH_A    0.50f
// 【删除】dir_raw未使用，直接使用frontDist/leftDist/rightDist
// volatile float dir_raw[NUM_DIR] = {400.0f, 400.0f, 400.0f};
volatile float dir_smt[NUM_DIR] = {400.0f, 400.0f, 400.0f};
// ==================== 智能避障参数（用户算法）====================
#define STEER_MAX_PWM 230
#define STEER_SLOW_PWM 180
#define FRONT_CRITICAL 80.0    // 【修复】与告警阈值一致，避免冲突
#define SIDE_WARNING   80.0    // 【修复】与前方阈值相同，公平比较

// 【新增】有效的雷达距离范围
#define RADAR_MIN_VALID_CM 20.0
#define RADAR_MAX_VALID_CM 400.0

float frontDist = 400.0;  // 【修复】初始值改为最大值
float leftDist  = 400.0;
float rightDist = 400.0;
// 【修复】电机当前功率（避障/雷达任务写入，VAD录音读取：电机运行噪声巨大，跳过触发）
volatile int motorPowerNow = 0;
// ==================== 电机控制 ====================
// ==================== 电机控制（正数右转，负数左转）====================
void motorControl(int steerPower) {
    int safePower = constrain(steerPower, -STEER_MAX_PWM, STEER_MAX_PWM);
    // 【修复】电机方向反了：实物转向与代码意图相反，把 IN1/IN2 电平对调。
    // 若对调后仍反（比如还伴随别的症状），说明不是极性问题，改回并把现象说清楚。
    if (safePower > 15) {
        // 👉 产生向右的动力
        digitalWrite(MOTOR_IN1, HIGH);
        digitalWrite(MOTOR_IN2, LOW);
        analogWrite(MOTOR_PWM, safePower);
        last_motor_dir = "right";
    }
    else if (safePower < -15) {
        // 👈 产生向左的动力
        digitalWrite(MOTOR_IN1, LOW);
        digitalWrite(MOTOR_IN2, HIGH);
        analogWrite(MOTOR_PWM, abs(safePower));
        last_motor_dir = "left";
    }
    else {
        // 🛑 危机解除，释放电机滑行
        digitalWrite(MOTOR_IN1, LOW);
        digitalWrite(MOTOR_IN2, LOW);
        analogWrite(MOTOR_PWM, 0);
        last_motor_dir = "stop";
    }

    // 【新增】调试输出电机状态
    static unsigned long lastMotorDebug = 0;
    if (millis() - lastMotorDebug > 500) {
        lastMotorDebug = millis();
        if (safePower != 0) {
            Serial.printf("[电机] 方向:%s 功率:%d 雷达F:%.0f L:%.0f R:%.0f\n",
                          last_motor_dir.c_str(), abs(safePower), frontDist, leftDist, rightDist);
        }
    }
    // 【修复】记录当前电机功率，VAD录音据此跳过（电机噪声能量1000+远超语音，不能识别）
    motorPowerNow = safePower;
}
// ==================== 避障决策（修复版 - 智能左右权重避障算法）====================
void smartAvoid() {
    float f = dir_smt[0];
    float L = dir_smt[1];
    float R = dir_smt[2];

    // 【修复】转向迟滞：避免雷达数据波动导致方向频繁切换
    static int lastSteerDir = 0;  // -1=左转, 1=右转, 0=直行/停止
    const float TURN_CHANGE_MARGIN = 30.0f;  // 切换方向需要的差距

    bool leftBlocked = (L < SIDE_WARNING);   // 80cm
    bool rightBlocked = (R < SIDE_WARNING);
    bool leftTight = (L < 40.0f);
    bool rightTight = (R < 40.0f);

    // 【修复】被包围日志只在进入/脱离状态变化时打印一次，避免每雷达周期刷屏淹没K230/TTS/ASR诊断日志
    static bool wasSurrounded = false;
    bool surrounded = (f < FRONT_CRITICAL) && leftTight && rightTight;
    if (surrounded != wasSurrounded) {
        wasSurrounded = surrounded;
        Serial.printf(surrounded ? "[避障] 进入被包围(F:%.0f L:%.0f R:%.0f)，停止\n"
                                 : "[避障] 脱离包围(F:%.0f L:%.0f R:%.0f)\n", f, L, R);
    }

    // 【修复】"前方XXcm，左转/右转"只在转向方向变化时打印一次（此前每雷达周期刷屏，识别/导航日志全被冲走）
    static int lastAvoidPrintDir = 0;   // -1=打印过左转 1=打印过右转 0=未打印/已停止

    // ===== 前方有障碍物 =====
    if (f < FRONT_CRITICAL) {
        // 两侧都非常堵(<40cm)才停止
        if (leftTight && rightTight) {
            motorControl(0);
            lastSteerDir = 0;
            lastAvoidPrintDir = 0;
            return;
        }
        // 决定转向方向（带迟滞）
        int desiredDir;
        if (L > R + TURN_CHANGE_MARGIN) {
            desiredDir = -1;  // 左边明显更空，左转
        } else if (R > L + TURN_CHANGE_MARGIN) {
            desiredDir = 1;   // 右边明显更空，右转
        } else {
            // 差距不大，保持上次方向（避免频繁切换），默认右转
            desiredDir = (lastSteerDir != 0) ? lastSteerDir : 1;
        }
        lastSteerDir = desiredDir;

        int power = (f < 50.0f) ? STEER_MAX_PWM : STEER_SLOW_PWM;
        if (desiredDir == -1) {
            motorControl(-power);
            if (lastAvoidPrintDir != -1) {
                lastAvoidPrintDir = -1;
                Serial.printf("[避障] 前方%.0fcm，左转(左%.0f vs 右%.0f)\n", f, L, R);
            }
        } else {
            motorControl(power);
            if (lastAvoidPrintDir != 1) {
                lastAvoidPrintDir = 1;
                Serial.printf("[避障] 前方%.0fcm，右转(左%.0f vs 右%.0f)\n", f, L, R);
            }
        }
        return;
    }

    // ===== 前方安全，侧边处理（带迟滞）=====
    if (leftBlocked && rightBlocked) {
        motorControl(0);
        lastSteerDir = 0;
        lastAvoidPrintDir = 0;
        return;
    }
    if (leftBlocked && !rightBlocked) {
        motorControl(STEER_SLOW_PWM);  // 左堵→右转
        lastSteerDir = 1;
        return;
    }
    if (rightBlocked && !leftBlocked) {
        motorControl(-STEER_SLOW_PWM);  // 右堵→左转
        lastSteerDir = -1;
        return;
    }
    // 无障碍物 → 停机
    lastSteerDir = 0;
    lastAvoidPrintDir = 0;
    motorControl(0);
}
// ==================== 雷达处理 ====================
// ==================== K230视觉检测处理 ====================
int getK230TargetIndex(const char* targetName) {
for (int i = 0; i < K230_ALERT_TARGET_COUNT; i++) {
if (strcmp(targetName, K230_ALERT_TARGETS[i]) == 0) {
return i;
}
}
return -1;
}
const char* getK230ChineseName(const char* targetName) {
if (strcmp(targetName, "red_light") == 0) return "红灯";
if (strcmp(targetName, "green_light") == 0) return "绿灯";
if (strcmp(targetName, "yellow_light") == 0) return "黄灯";
if (strcmp(targetName, "stairs") == 0) return "台阶";
if (strcmp(targetName, "person") == 0) return "行人";
if (strcmp(targetName, "ashcan") == 0) return "垃圾桶";
if (strcmp(targetName, "curb") == 0) return "路缘石";
if (strcmp(targetName, "blind_track") == 0) return "盲道";
if (strcmp(targetName, "crosswalk") == 0) return "斑马线";
if (strcmp(targetName, "pole") == 0) return "立柱";
if (strcmp(targetName, "reflective_cone") == 0) return "反光锥";
if (strcmp(targetName, "stop_sign") == 0) return "标志牌";
if (strcmp(targetName, "vehicle") == 0) return "车辆";
if (strcmp(targetName, "puddle") == 0) return "水坑";
return targetName;
}
// 解析K230多目标检测数据 DETS:class,conf,x,y,w,h;...
void parseK230MultiDetections(String& data) {
k230_detection_count = 0;
// 去除 "DETS:" 前缀
String payload = data.substring(5);
// 按分号分割多个目标
int start = 0;
while (start < payload.length() && k230_detection_count < MAX_K230_DETECTIONS) {
int end = payload.indexOf(';', start);
if (end == -1) end = payload.length();
String targetStr = payload.substring(start, end);
// 按逗号分割: class,conf,x,y,w,h
int comma1 = targetStr.indexOf(',');
int comma2 = targetStr.indexOf(',', comma1 + 1);
int comma3 = targetStr.indexOf(',', comma2 + 1);
int comma4 = targetStr.indexOf(',', comma3 + 1);
int comma5 = targetStr.indexOf(',', comma4 + 1);
if (comma1 > 0 && comma2 > comma1 && comma3 > comma2 && comma4 > comma3 && comma5 > comma4) {
String cls = targetStr.substring(0, comma1);
float conf = targetStr.substring(comma1 + 1, comma2).toFloat();
int x = targetStr.substring(comma2 + 1, comma3).toInt();
int y = targetStr.substring(comma3 + 1, comma4).toInt();
int w = targetStr.substring(comma4 + 1, comma5).toInt();
int h = targetStr.substring(comma5 + 1).toInt();
k230_detections[k230_detection_count].targetClass = cls;
k230_detections[k230_detection_count].targetLabel = getK230ChineseName(cls.c_str());
k230_detections[k230_detection_count].x = x;
k230_detections[k230_detection_count].y = y;
k230_detections[k230_detection_count].w = w;
k230_detections[k230_detection_count].h = h;
k230_detections[k230_detection_count].confidence = conf;
k230_detections[k230_detection_count].timestamp = millis();
k230_detection_count++;
}
start = end + 1;
}
}
// 解析K230单目标检测数据 DET:class
void parseK230SingleDetection(String& data) {
k230_detection_count = 0;
String cls = data.substring(4); // 去除 "DET:" 前缀
k230_detections[0].targetClass = cls;
k230_detections[0].targetLabel = getK230ChineseName(cls.c_str());
k230_detections[0].x = 160;  // 默认中心位置
k230_detections[0].y = 160;
k230_detections[0].w = 100;  // 默认尺寸
k230_detections[0].h = 100;
k230_detections[0].confidence = 0.85;
k230_detections[0].timestamp = millis();
k230_detection_count = 1;
}
void sendK230_TTSRequest(const char* targetName) {
    Serial.printf("[K230播报] 目标:%s -> 发送TTS请求\n", targetName);  // 诊断
    if (!mqtt.connected()) { Serial.println("[K230播报] MQTT未连接，跳过"); return; }
StaticJsonDocument<256> ttsDoc;
char buf[256];
char alertText[64];
const char* chineseName = getK230ChineseName(targetName);
snprintf(alertText, sizeof(alertText), "前方有%s", chineseName);
ttsDoc["text"] = alertText;
ttsDoc["priority"] = PRIO_NORMAL;
size_t len = serializeJson(ttsDoc, buf, sizeof(buf));
// 【修复】短超时发布：本函数在 RadarTask 避障循环内调用，绝不让网络锁卡避障
mqttPublishShort(MQTT_TOPIC_TTS_REQ, buf, len);
}
/**
* 处理从K230接收的数据（仅检测数据）- 限制每次处理的最大字符数避免阻塞
*/
void processK230Data() {
    // 【诊断】每5秒打印K230串口状态，确认K230是否在发数据
    static unsigned long lastK230Diag = 0;
    if (millis() - lastK230Diag > 5000) {
        lastK230Diag = millis();
        Serial.printf("[K230诊断] 串口可用:%d 缓冲:%d\n", k230Serial.available(), k230_receiveBuffer.length());
    }
    // 【修改】每次尽量读空K230缓冲（上限1024字符），避障忙时K230数据不再被覆盖丢弃
    int maxChars = k230Serial.available();
    if (maxChars > 1024) maxChars = 1024;
    // 【修改】乱码可能让缓冲区堆积垃圾：超过512字符直接丢弃，防止无限增长
    if (k230_receiveBuffer.length() > 512) {
        k230_receiveBuffer = "";
    }
while (k230Serial.available() > 0 && maxChars-- > 0) {
char c = k230Serial.read();
if (c == '\n') {
k230_receiveBuffer.trim();
if (k230_receiveBuffer.length() > 0) {
// 【修改】乱码会包在有效帧前面（如 垃圾...DET:vehicle），只认行首会漏掉大部分帧。
// 改为在整行里搜索帧标记，取标记之后的子串解析，垃圾自动被丢弃
int detsIdx = k230_receiveBuffer.indexOf("DETS:");
int detIdx  = k230_receiveBuffer.indexOf("DET:");
int noneIdx = k230_receiveBuffer.indexOf("NONE");
if (detsIdx >= 0) {
    String frame = k230_receiveBuffer.substring(detsIdx);
    Serial.printf("[K230] 收到多目标: %s\n", frame.c_str());
parseK230MultiDetections(frame);
for (int i = 0; i < k230_detection_count; i++) {
int targetIndex = getK230TargetIndex(k230_detections[i].targetClass.c_str());
if (targetIndex >= 0) {
unsigned long now = millis();
if (now - k230_lastAlertTime[targetIndex] >= K230_ALERT_COOLDOWN_MS) {
sendK230_TTSRequest(k230_detections[i].targetClass.c_str());
k230_lastAlertTime[targetIndex] = now;
break;
}
}
}
} else if (detIdx >= 0) {
    String frame = k230_receiveBuffer.substring(detIdx);
    Serial.printf("[K230] 收到检测: %s\n", frame.c_str());
parseK230SingleDetection(frame);
String targetName = frame.substring(4);
int targetIndex = getK230TargetIndex(targetName.c_str());
if (targetIndex >= 0) {
unsigned long now = millis();
if (now - k230_lastAlertTime[targetIndex] >= K230_ALERT_COOLDOWN_MS) {
sendK230_TTSRequest(targetName.c_str());
k230_lastAlertTime[targetIndex] = now;
}
}
} else if (noneIdx >= 0) {
k230_detection_count = 0;
} else {
// 【新增】既不是DETS/DET也不是NONE：打印原始帧便于诊断K230格式异常（节流防刷屏）
static unsigned long lastUnknownK230 = 0;
if (millis() - lastUnknownK230 > 5000) {
lastUnknownK230 = millis();
Serial.printf("[K230] 未知帧(前80字符): %.80s\n", k230_receiveBuffer.c_str());
}
}
k230_receiveBuffer = "";
}
} else {
k230_receiveBuffer += c;
}
}
}
void processRadarPacket() {
    uint16_t fsa = payload_buf[0] | (payload_buf[1] << 8);
    uint16_t lsa = payload_buf[2] | (payload_buf[3] << 8);
    float angleFSA = (fsa >> 1) / 64.0f;
    float angleLSA = (lsa >> 1) / 64.0f;
    float diffAngle = angleLSA - angleFSA;
    if (diffAngle < 0) diffAngle += 360.0f;

    // 【修复】移除这里错误的重置逻辑，让数据自然更新
    // 原始代码在这里重置三个方向的距离是错误的，会导致数据丢失

    for (int i = 0; i < packet_lsn; i++) {
        uint16_t si = payload_buf[6 + i * 2] | (payload_buf[6 + i * 2 + 1] << 8);
        float distanceMm = si / 4.0f;

        // 【修复】添加有效性检查
        if (distanceMm > (RADAR_MIN_VALID_CM * 10.0f) && distanceMm < (RADAR_MAX_VALID_CM * 10.0f)) {
            float cm = distanceMm / 10.0f;
            float currentAngle = angleFSA;
            if (packet_lsn > 1) currentAngle += (diffAngle / (packet_lsn - 1)) * i;
            if (currentAngle >= 360.0f) currentAngle -= 360.0f;

            // 【修复】使用统一的扇区判断逻辑
            // 前方：330-360 或 0-30
            if (currentAngle >= ANG_FRONT_MIN || currentAngle <= ANG_FRONT_MAX) {
                if (cm < frontDist) frontDist = cm;
            }
            // 左方：60-120
            else if (currentAngle >= ANG_LEFT_MIN && currentAngle <= ANG_LEFT_MAX) {
                if (cm < leftDist) leftDist = cm;
            }
            // 右方：240-300
            else if (currentAngle >= ANG_RIGHT_MIN && currentAngle <= ANG_RIGHT_MAX) {
                if (cm < rightDist) rightDist = cm;
            }
        }
    }

    // 【新增】限制最大距离为400cm，避免无效数据
    if (frontDist > RADAR_MAX_VALID_CM) frontDist = RADAR_MAX_VALID_CM;
    if (leftDist > RADAR_MAX_VALID_CM) leftDist = RADAR_MAX_VALID_CM;
    if (rightDist > RADAR_MAX_VALID_CM) rightDist = RADAR_MAX_VALID_CM;

    // EMA平滑处理
    dir_smt[0] = SMOOTH_A * frontDist + (1.0f - SMOOTH_A) * dir_smt[0];
    dir_smt[1] = SMOOTH_A * leftDist + (1.0f - SMOOTH_A) * dir_smt[1];
    dir_smt[2] = SMOOTH_A * rightDist + (1.0f - SMOOTH_A) * dir_smt[2];

    // 调试输出雷达数据（每1秒输出一次）
    static unsigned long lastRadarDebug = 0;
    if (millis() - lastRadarDebug > 1000) {
        lastRadarDebug = millis();
        Serial.printf("[雷达数据] 原始:前:%.0fcm 左:%.0fcm 右:%.0fcm | 平滑:前:%.0fcm 左:%.0fcm 右:%.0fcm\n",
                      frontDist, leftDist, rightDist, dir_smt[0], dir_smt[1], dir_smt[2]);
    }
}
void parseGPSNMEA() {
static char nmea[256];
static uint8_t idx = 0;
static unsigned long lastNmeaDump = 0;
while (gpsSerial.available()) {
char c = gpsSerial.read();
gps_byte_count++;
if (c == '$') { idx = 0; nmea[idx++] = c; }
else if (idx > 0 && idx < 255) {
nmea[idx++] = c;
if (c == '\n' || c == '\r') {
nmea[idx] = '\0';
// 【修复】只要收到完整的$开头NMEA句子就认为波特率正确并锁定，
// 不必等解析到带经纬度的GGA（无卫星定位时经纬度字段为空，sscanf会失败导致永不锁定）
if (nmea[0] == '$') gps_got_nmea = true;
// 调试：每5秒打印收到的NMEA原文（判断是否有GPS数据进来）
if (millis() - lastNmeaDump > 5000) {
lastNmeaDump = millis();
Serial.printf("[GPS-NMEA] %s\n", nmea);
}
// 解析 GGA 语句：位置 + 卫星数
if (strstr(nmea, "GGA") != NULL) {
float lat_raw = 0, lng_raw = 0;
char ns = 'N', ew = 'E';
int fix = 0, sats = 0;
if (sscanf(nmea, "$%*[^,],%*[^,],%f,%c,%f,%c,%d,%d,", &lat_raw, &ns, &lng_raw, &ew, &fix, &sats) >= 6) {
// 只有成功解析出GGA字段才算波特率正确
gps_got_nmea = true;
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
// ==================== MQTT 重连（非阻塞）====================
// 返回true表示连接成功，false表示正在重连中
bool mqtt_reconnect_nonblocking() {
static unsigned long last_retry = 0;
static int retry_count = 0;
const unsigned long RETRY_INTERVAL = 3000; // 3秒重试间隔
if (mqtt.connected()) return true;
unsigned long now = millis();
if (now - last_retry < RETRY_INTERVAL) return false;
last_retry = now;
// 确保WiFi连接状态正常
if (WiFi.status() != WL_CONNECTED) return false;
// 配置MQTT客户端参数
mqtt.setSocketTimeout(5);   // 【修复】10→5秒：RadarTask跑在CPU0，socket读超时太长会饿死IDLE0触发task_wdt
mqtt.setKeepAlive(60);
// 【修复】MQTT Buffer=8KB：既要容纳TTS音频分片(每片4KB)，又不会像128KB那样吃光内存
mqtt.setBufferSize(8192);
espClient.setInsecure();
espClient.setHandshakeTimeout(3);  // 【修复】12→3秒：TLS握手是CPU0忙循环，必须<task_wdt的5秒，否则握手时IDLE0饿死、系统Abort
if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
Serial.println("[MQTT] 连接成功");
mqtt.subscribe(MQTT_TOPIC_TTS_AUDIO);
mqtt.subscribe("blindstick/tts/control");
mqtt.subscribe("blindstick/tts/stream/+");
mqtt.subscribe(MQTT_TOPIC_NAV_STEPS);
mqtt.subscribe(MQTT_TOPIC_TTS_REQ);
mqtt.subscribe("blindstick/config/home_city");
mqtt.subscribe("blindstick/stats/detour");
mqtt.subscribe("blindstick/asr/result");
retry_count = 0;
// 【开机语音】只在系统启动后的首次MQTT连接时播放一次
if (!startup_announced && !startup_announced_rtc) {
delay(100);  // 短暂延迟确保连接稳定
playStartupVoice();
startup_announced = true;
startup_announced_rtc = true;
}
return true;
} else {
Serial.printf("[MQTT] 连接失败 state=%d（-4超时 -2失败 5未授权 4凭据错 2客户端ID）\n", mqtt.state());
retry_count++;
if (retry_count >= 10) retry_count = 0;
return false;
}
}
// 旧的重连函数（保留兼容性，但内部调用非阻塞版本）
void mqtt_reconnect() {
mqtt_reconnect_nonblocking();
}
// 【ASR over MQTT】识别结果缓冲（由blindstick/asr/result消息填充，VoiceTask等待读取）
char asr_result_text[512] = "";
volatile bool asr_result_ready = false;
// 【地图 over MQTT】地点/路线结果缓冲（proxy压缩后的JSON，两个请求串行共用）
// 【修复】地点/路线结果分缓冲，避免超时后迟到的旧结果交叉污染（两者JSON格式不同）
char places_result_buf[4096] = "";
volatile bool places_result_ready = false;
char directions_result_buf[4096] = "";
volatile bool directions_result_ready = false;
// ==================== MQTT 消息回调（支持流式TTS）====================
void mqtt_callback(char* topic, byte* payload, unsigned int length) {
// ===== 流式TTS控制消息处理（最高优先级）=====
if (strcmp(topic, "blindstick/tts/control") == 0) {
handleStreamControl((const char*)payload, length);
return;
}
// ===== ASR识别结果（Render MQTT回传，VoiceTask等待读取）=====
if (strcmp(topic, "blindstick/asr/result") == 0) {
StaticJsonDocument<768> doc;
DeserializationError err = deserializeJson(doc, payload, length);
if (!err) {
int errNo = doc["err_no"] | -1;
if (errNo == 0) {
const char* text = doc["text"] | "";
strncpy(asr_result_text, text, sizeof(asr_result_text) - 1);
asr_result_text[sizeof(asr_result_text) - 1] = '\0';
asr_result_ready = true;
Serial.printf("[语音诊断] ASR结果: %s\n", asr_result_text);
} else {
const char* msg = doc["msg"] | "";
Serial.printf("[语音诊断] ASR错误 err_no=%d msg=%s\n", errNo, msg);
}
}
return;
}
// ===== 地点搜索结果（proxy压缩后JSON，VoiceTask等待读取）=====
if (strcmp(topic, "blindstick/places/result") == 0) {
strncpy(places_result_buf, (const char*)payload, sizeof(places_result_buf) - 1);
places_result_buf[sizeof(places_result_buf) - 1] = '\0';
places_result_ready = true;
Serial.printf("[导航] 地点搜索结果: %d字节\n", length);
return;
}
// ===== 步行路线结果（proxy压缩后JSON，VoiceTask等待读取）=====
if (strcmp(topic, "blindstick/directions/result") == 0) {
strncpy(directions_result_buf, (const char*)payload, sizeof(directions_result_buf) - 1);
directions_result_buf[sizeof(directions_result_buf) - 1] = '\0';
directions_result_ready = true;
Serial.printf("[导航] 路线规划结果: %d字节\n", length);
return;
}
// ===== 流式TTS音频分片（Render MQTT推送，重装后由独立任务播放）=====
if (strncmp(topic, "blindstick/tts/stream/", 22) == 0) {
appendStreamChunk(payload, length);
return;
}
// ===== 常住地设置处理 =====
if (strcmp(topic, "blindstick/config/home_city") == 0) {
StaticJsonDocument<256> doc;
DeserializationError err = deserializeJson(doc, payload, length);
if (!err && doc.containsKey("city")) {
const char* new_city = doc["city"];
home_city = String(new_city);
// 【修复】写入NVS持久化，重启/烧录后仍保留
homeCityPrefs.putString("city", home_city);
// 播报确认
StaticJsonDocument<256> ttsDoc;
char confirmText[64];
snprintf(confirmText, sizeof(confirmText), "常住地已设置为%s", home_city.c_str());
ttsDoc["text"] = confirmText;
ttsDoc["priority"] = PRIO_NORMAL;
char buf[256];
size_t len = serializeJson(ttsDoc, buf, sizeof(buf));
mqttPublish("blindstick/tts/request", buf, len);
}
return;
}
// ===== 路线调整统计处理 =====
if (strcmp(topic, "blindstick/stats/detour") == 0) {
detour_count++;
saveStatsToRTC();
return;
}
// ===== 原有的TTS_AUDIO处理（兼容旧版）=====
if (strcmp(topic, MQTT_TOPIC_TTS_AUDIO) == 0) {
if (length < 1000 || length >= TTS_AUDIO_BUF_SIZE) {
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
// 【修复】4096→static：mqtt_callback 在 RadarTask 栈上执行（经 mqttLoop），4KB文档是栈溢出元凶之一
static StaticJsonDocument<4096> doc;
DeserializationError err = deserializeJson(doc, payload, length);
if (!err && doc["status"] == "ok") {
JsonArray steps = doc["steps"];
nav_total_steps = 0;
for (const char* step : steps) {
nav_steps[nav_total_steps] = String(step);
nav_total_steps++; if (nav_total_steps >= 10) break;
}
current_step_idx = 0; current_progress = 0; nav_active = true;
// 增加导航次数统计
nav_count++;
saveStatsToRTC();
}
} else if (strcmp(topic, MQTT_TOPIC_TTS_REQ) == 0) {
// 【注意】ESP32不再转发TTS请求到代理服务器
// 代理服务器直接订阅 blindstick/tts/request，无需ESP32转发
// 这避免了MQTT消息循环问题
// 如果需要本地处理TTS请求，可以在这里添加代码
}
}
// 全局变量
// 【删除】last_blocked未使用
unsigned long last_alert_time = 0;
// 【删除】last_alert_dist未使用
// 【加快】障碍物告警间隔从8秒改为5秒
#define ALERT_INTERVAL_MS 5000
#define ALERT_DIST_CHANGE 30          // 距离变化超过30cm才重新播报
// 避障语音去重：记录上次播报的文本和时间
static String last_alert_text = "";
static unsigned long last_alert_text_time = 0;
// 【加快】去重时间从10秒改为5秒
#define ALERT_TEXT_DUPLICATE_MS 5000
// 【新增】避障播报固定间隙：无论文本/距离如何变化，8秒内最多播报一次
// 【修复】极端危险(障碍物极近)也设独立间隙(3秒)，防止"<40cm一直触发"导致连续播报/打断TTS/队列满
#define AVOID_ALERT_MIN_INTERVAL_MS 8000      // 普通告警：8秒一次
#define AVOID_ALERT_CRITICAL_INTERVAL_MS 3000 // 极端危险：3秒一次（仍允许打断当前TTS，但不再每帧都播）
static unsigned long lastAvoidAlertTime = 0;
// 【修复】极端危险判定：前方<30cm 或 侧方<25cm 才算（原<40/35太宽，31~39cm一直触发绕过间隙）
#define CRITICAL_DANGER_F_CM 30.0f
#define CRITICAL_DANGER_S_CM 25.0f
static inline bool isCriticalDanger(float f, float L, float R) {
    return (f < CRITICAL_DANGER_F_CM) || (L < CRITICAL_DANGER_S_CM) || (R < CRITICAL_DANGER_S_CM);
}
// 【优化】TTS触发阈值
#define TTS_TRIGGER_DISTANCE_CM 60     // 距离小于60cm触发紧急播报
// 【加快】连续检测从2次改为1次，更快响应
#define TTS_TRIGGER_COUNT 1
// ==================== 障碍物检测和播报（三向雷达版 - 修复版）====================
void checkObstacleAndAlert() {
    // 【新增】检查 is_ai_talking 是否超时卡住
    if (is_ai_talking && (millis() - ai_talking_start_time > AI_TALKING_TIMEOUT_MS)) {
        Serial.println("[TTS超时] is_ai_talking 超时，自动重置");
        is_ai_talking = false;
    }

    // 三向雷达: [0]=前方, [1]=左方, [2]=右方
    float f = dir_smt[0];
    float L = dir_smt[1];
    float R = dir_smt[2];

    // 【修复】调整阈值，使用更合理的告警距离
    const float FRONT_ALERT_CM = 100.0f;  // 前方告警阈值
    const float SIDE_ALERT_CM = 80.0f;    // 侧边告警阈值

    unsigned long now = millis();

    // 【修复】独立的连续检测计数器，每个方向单独计数
    static int frontConsecutive = 0;
    static int leftConsecutive = 0;
    static int rightConsecutive = 0;

    // 检测各方向障碍物
    bool frontHasObstacle = (f < FRONT_ALERT_CM);
    bool leftHasObstacle = (L < SIDE_ALERT_CM);
    bool rightHasObstacle = (R < SIDE_ALERT_CM);

    // 更新连续计数器（每方向独立）
    if (frontHasObstacle) frontConsecutive++; else frontConsecutive = 0;
    if (leftHasObstacle) leftConsecutive++; else leftConsecutive = 0;
    if (rightHasObstacle) rightConsecutive++; else rightConsecutive = 0;

    // 【修复】简化触发逻辑：只要有任意方向连续检测到2次就触发
    bool shouldAlert = (frontConsecutive >= 2) || (leftConsecutive >= 2) || (rightConsecutive >= 2);

    // 【修复】强制播报：如果距离小于60cm，立即播报（不等待连续检测）
    bool urgentAlert = (f < 60.0f) || (L < 50.0f) || (R < 50.0f);

    // 调试输出（每2秒一次）
    static unsigned long lastObstacleDebug = 0;
    if (now - lastObstacleDebug > 2000) {
        lastObstacleDebug = now;
        Serial.printf("[避障检测] F:%.0f(%s) L:%.0f(%s) R:%.0f(%s) 连续:%d/%d/%d\n",
                      f, frontHasObstacle ? "警告" : "正常",
                      L, leftHasObstacle ? "警告" : "正常",
                      R, rightHasObstacle ? "警告" : "正常",
                      frontConsecutive, leftConsecutive, rightConsecutive);
    }

    if (!shouldAlert && !urgentAlert) {
        return;  // 没有需要播报的情况
    }

    // 【新增】播放后静默期检查：TTS刚播放完，给语音识别完整录音窗口（除非极端危险）
    if (millis() - ttsPlayEndTime < TTS_PLAY_SILENT_MS) {
        bool criticalDanger = isCriticalDanger(f, L, R);
        if (!criticalDanger) {
            return;  // 静默期内且非极端危险，跳过本次播报
        }
    }

    // 【修复】检查是否正在播报其他内容
    // 【修复】正在播放TTS时抑制新的避障播报，避免连续播报导致语音识别被一直挂起
    // 仅当距离极度危险(<30cm)时才打断当前播放紧急播报
    if (is_ai_talking || getTTSRequesting()) {
        bool criticalDanger = isCriticalDanger(f, L, R);
        if (!criticalDanger) {
            return;  // 正在播报且非极端危险，跳过本次（下轮再试），给语音识别留时间
        }
        // 【修复】自环保护：当前播的已经是避障告警(PRIO_HIGH)时，不打断自己，
        //         否则会陷入"播报→打断→重新播报"的死循环刷屏。
        if (is_ai_talking && stream_priority >= PRIO_HIGH) {
            return;  // 已在播极端告警，等它播完即可
        }
        // 极端危险且正在播低优先级内容（如语音回复）：发送打断信号，让避障播报优先
        if (is_ai_talking) {
            StaticJsonDocument<256> doc;
            doc["type"] = "interrupt";
            doc["priority"] = PRIO_HIGH;
            char buf[256];
            size_t len = serializeJson(doc, buf, sizeof(buf));
            mqttPublish("blindstick/tts/control", buf, len);
            delay(50);
        }
    }

    // 构建告警文本
    String alert_text = "";

    // 【修复】优先级1：前方障碍物（最紧急）
    if (frontConsecutive >= 2 || f < 60.0f) {
        if (L > R + 30.0f) {
            alert_text = "前方有障碍物，请向左绕行";
        } else if (R > L + 30.0f) {
            alert_text = "前方有障碍物，请向右绕行";
        } else if (L > SIDE_ALERT_CM && R > SIDE_ALERT_CM) {
            // 两边都有空间，选择更空的一边
            alert_text = (L > R) ? "前方有障碍物，建议向左绕行" : "前方有障碍物，建议向右绕行";
        } else if (L > SIDE_ALERT_CM) {
            alert_text = "前方有障碍物，请向左绕行";
        } else if (R > SIDE_ALERT_CM) {
            alert_text = "前方有障碍物，请向右绕行";
        } else {
            alert_text = "前方和两侧都有障碍物，请小心慢行";
        }
    }
    // 优先级2：左方障碍物（仅当前方安全时）
    else if (leftConsecutive >= 2 || L < 50.0f) {
        if (R > SIDE_ALERT_CM) {
            alert_text = "左方有障碍物，请向右绕行";
        } else {
            alert_text = "左方有障碍物，请注意避让";
        }
    }
    // 优先级3：右方障碍物（仅当前方安全时）
    else if (rightConsecutive >= 2 || R < 50.0f) {
        if (L > SIDE_ALERT_CM) {
            alert_text = "右方有障碍物，请向左绕行";
        } else {
            alert_text = "右方有障碍物，请注意避让";
        }
    }

    if (alert_text.length() == 0) {
        return;
    }

    // 【新增】固定播报间隙：普通告警8秒一次，极端危险(<30cm)3秒一次
    // 【修复】原实现"极端危险完全绕过间隙"导致前方31~39cm持续触发、连续打断TTS、队列满
    //        现在临界危险也保留3秒短间隙，既不丢紧急提醒，也不再每帧轰炸
    bool criticalDanger = isCriticalDanger(f, L, R);
    unsigned long minInterval = criticalDanger ? AVOID_ALERT_CRITICAL_INTERVAL_MS : AVOID_ALERT_MIN_INTERVAL_MS;
    if ((now - lastAvoidAlertTime) < minInterval) {
        return;  // 间隙内，跳过本次播报
    }

    // 【修复】去重检查 - 基于告警文本内容
    static String lastAlertText = "";
    static unsigned long lastAlertTime = 0;
    static float lastAlertFront = 0;
    static float lastAlertLeft = 0;
    static float lastAlertRight = 0;
    const unsigned long ALERT_COOLDOWN_MS = 8000;  // 【修复】8秒内不重复相同告警，给语音识别留时间

    if (alert_text == lastAlertText && (now - lastAlertTime) < ALERT_COOLDOWN_MS) {
        return;  // 相同告警在冷却期内，跳过
    }

    // 【修复】检查距离变化，如果距离变化很小也跳过
    float distChange = abs(f - lastAlertFront) + abs(L - lastAlertLeft) + abs(R - lastAlertRight);
    if (distChange < 50.0f && (now - lastAlertTime) < ALERT_COOLDOWN_MS * 2) {
        // 距离变化不大，延长冷却期
        return;
    }

    // 发送TTS请求
    if (mqtt.connected()) {
        Serial.printf("[避障播报] %s (F:%.0f L:%.0f R:%.0f)\n", alert_text.c_str(), f, L, R);

        StaticJsonDocument<256> ttsDoc;
        char buf[256];
        ttsDoc["text"] = alert_text;
        ttsDoc["priority"] = PRIO_HIGH;  // 避障使用高优先级
        size_t len = serializeJson(ttsDoc, buf, sizeof(buf));

        if (mqttPublish(MQTT_TOPIC_TTS_REQ, buf, len)) {
            // 更新记录
            lastAlertText = alert_text;
            lastAlertTime = now;
            lastAlertFront = f;
            lastAlertLeft = L;
            lastAlertRight = R;
            lastAvoidAlertTime = now;  // 【新增】记录本次播报时间，用于固定间隙

            // 重置连续计数器
            frontConsecutive = 0;
            leftConsecutive = 0;
            rightConsecutive = 0;

            // 增加障碍物提醒次数统计
            obstacle_count++;
            saveStatsToRTC();

            // 【修复】标记正在请求TTS，让"正在播报"抑制逻辑生效
            // （之前is_tts_requesting从未被设为true，导致避障播报不受抑制，
            //   VoiceTask被频繁挂起，语音识别无法工作）
            setTTSRequesting(true);
        }
    }
}
// ==================== TTS请求超时时间 ====================
#define TTS_REQUEST_TIMEOUT_MS 10000
// ==================== 辅助函数：使用PSRAM或普通内存分配 ====================
void* allocateBuffer(size_t size) {
size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
if (psram_free > size + 10000) {
return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
} else {
return malloc(size);
}
}
// ==================== 保存统计数据到RTC内存 ====================
void saveStatsToRTC() {
rtc_total_mileage = (uint32_t)total_mileage;
rtc_nav_count = nav_count;
rtc_obstacle_count = obstacle_count;
rtc_detour_count = detour_count;
if (has_last_gps_pos) {
rtc_last_gps_lat = *((uint32_t*)&last_gps_lat_for_mileage);
rtc_last_gps_lng = *((uint32_t*)&last_gps_lng_for_mileage);
rtc_has_last_pos = true;
}
}
// ==================== 计算两点间距离（米）====================
float calcDistanceFloat(float lat1, float lng1, float lat2, float lng2) {
const float R = 6371000; // 地球半径（米）
float dLat = (lat2 - lat1) * PI / 180.0;
float dLng = (lng2 - lng1) * PI / 180.0;
float a = sin(dLat/2) * sin(dLat/2) +
cos(lat1 * PI / 180.0) * cos(lat2 * PI / 180.0) *
sin(dLng/2) * sin(dLng/2);
float c = 2 * atan2(sqrt(a), sqrt(1-a));
return R * c;
}
// ==================== 更新里程统计 ====================
void updateMileage() {
if (gps_lat > 1.0 && gps_lng > 1.0) {
if (has_last_gps_pos) {
float dist = calcDistanceFloat(last_gps_lat_for_mileage, last_gps_lng_for_mileage, gps_lat, gps_lng);
if (dist > 1.0 && dist < 100.0) {  // 过滤跳变和静止
total_mileage += dist;
saveStatsToRTC();  // 保存到RTC内存
}
}
last_gps_lat_for_mileage = gps_lat;
last_gps_lng_for_mileage = gps_lng;
has_last_gps_pos = true;
}
}
static char json_buffer[2048];  // 扩大到2KB以容纳视觉检测数据
// ==================== 通过 MQTT 发布传感器数据 ====================
void publishSensorData() {
if (!mqtt.connected()) return;
// 更新里程统计
updateMileage();
// 使用更大的JSON缓冲区容纳视觉检测数据
// 【修复】2048→static：publishSensorData 在 RadarTask 栈上，2KB文档腾出栈空间防溢出（仅RadarTask调用，static安全）
static StaticJsonDocument<2048> doc;  // 【修复】1024→2048：K230多目标时k230_dets数组会占满1024池，ArduinoJson静默丢框
doc["device_id"] = "blind_stick_001";
JsonObject radar = doc.createNestedObject("radar");
// 三向雷达: [0]=前方, [1]=左方, [2]=右方
radar["f"] = dir_smt[0];  // 前方
radar["l"] = dir_smt[1];  // 左方
radar["r"] = dir_smt[2];  // 右方
// 【修复】blocked状态：前方<180cm或侧边<180cm时为true
is_blocked = (dir_smt[0] < 180.0 || dir_smt[1] < 180.0 || dir_smt[2] < 180.0);
doc["blocked"] = is_blocked;
doc["nav"] = nav_active;
doc["nav_destination"] = current_destination;  // 【新增】当前导航目的地
doc["nav_step"] = nav_active ? nav_steps[current_step_idx] : "";  // 【新增】当前步骤
doc["current_step"] = current_step_idx;  // 【新增】当前步骤索引
doc["nav_steps"] = nav_total_steps;  // 【新增】总步骤数
JsonObject gps = doc.createNestedObject("gps");
gps["lat"] = gps_lat;
gps["lng"] = gps_lng;
gps["satellites"] = gps_satellites;  // 统一字段名与前端一致
gps["speed"] = gps_speed;            // 添加速度字段
gps["bytes"] = gps_byte_count;       // GPS累计接收字节数（诊断：>0说明有信号进来）
// 调试输出（每5秒一次）
static unsigned long lastPublishDebug = 0;
if (millis() - lastPublishDebug > 5000) {
lastPublishDebug = millis();
Serial.printf("[MQTT上传] 雷达 F:%.0f L:%.0f R:%.0f GPS:%.6f,%.6f\n",
dir_smt[0], dir_smt[1], dir_smt[2], gps_lat, gps_lng);
}
// K230视觉检测数据 - 发送最新检测到的目标
if (k230_detection_count > 0 && millis() - k230_detections[0].timestamp < 5000) {
doc["k230_class"] = k230_detections[0].targetClass;
doc["k230_label"] = k230_detections[0].targetLabel;
// 【修复】发送真实坐标到网页（不再写死居中框），格式: [{class,label,x,y,w,h,conf}]
JsonArray dets = doc.createNestedArray("k230_dets");
int detCount = k230_detection_count > MAX_K230_DETECTIONS ? MAX_K230_DETECTIONS : k230_detection_count;
for (int i = 0; i < detCount; i++) {
JsonObject d = dets.createNestedObject();
d["class"] = k230_detections[i].targetClass;
d["label"] = k230_detections[i].targetLabel;
d["x"] = k230_detections[i].x;
d["y"] = k230_detections[i].y;
d["w"] = k230_detections[i].w;
d["h"] = k230_detections[i].h;
d["conf"] = k230_detections[i].confidence;
}
// 是否属于危险目标（用于前端红色高亮，取第一个）
bool isDanger = (k230_detections[0].targetClass == "red_light" ||
k230_detections[0].targetClass == "person" ||
k230_detections[0].targetClass == "vehicle" ||
k230_detections[0].targetClass == "stairs" ||
k230_detections[0].targetClass == "puddle");
doc["k230_danger"] = isDanger;
} else {
doc["k230_class"] = "none";
doc["k230_label"] = "";
doc["k230_danger"] = false;
doc["k230_dets"].to<JsonArray>();
}
// 添加今日出行统计数据
JsonObject stats = doc.createNestedObject("stats");
stats["total_mileage"] = (int)total_mileage;   // 总里程（米）
stats["nav_count"] = nav_count;                 // 导航次数
stats["obstacle_count"] = obstacle_count;       // 障碍物提醒次数
stats["detour_count"] = detour_count;           // 路线调整次数
size_t n = serializeJson(doc, json_buffer, sizeof(json_buffer));
if (n == 0 || n >= sizeof(json_buffer)) {
Serial.printf("[MQTT] JSON序列化失败或过大: %d字节\n", n);
return;
}
// 【调试】每10秒打印一次JSON大小
static unsigned long lastJsonDebug = 0;
if (millis() - lastJsonDebug > 10000) {
lastJsonDebug = millis();
Serial.printf("[MQTT] 发布JSON %d字节\n", n);
}
bool published = mqttPublish(MQTT_TOPIC_SENSORS, (const uint8_t*)json_buffer, n, false);
if (!published) {
Serial.println("[MQTT] 发布失败");
}
}
void RadarMotorUploadTask(void* pvParameters) {
Serial.println("[任务] RadarMotorUploadTask 启动");
// 雷达使用Serial1（硬件串口）
Serial1.begin(115200, SERIAL_8N1, RADAR_RX_PIN, -1);
Serial.printf("[雷达] Serial1初始化 RX=GPIO%d 波特率=115200\n", RADAR_RX_PIN);
// 【修复】发送YDLIDAR启动命令，让雷达开始扫描输出数据（之前只启动电机没发命令，导致收不到数据全是400）
delay(200);  // 等待串口就绪
Serial1.write(YDLIDAR_CMD_START, sizeof(YDLIDAR_CMD_START));
Serial.println("[雷达] 已发送YDLIDAR启动命令");
// GPS改为软串口（波特率自动检测）
gps_baud_index = 0;
gpsSerial.begin(gps_baud_table[gps_baud_index]);
gpsSerial.listen();  // 必须listen才会开始接收RX数据
gps_baud_try_start = millis();
Serial.printf("[GPS] 软串口初始化，尝试波特率=%d\n", gps_baud_table[gps_baud_index]);
// K230硬件串口UART2
// 【修复】加大K230串口接收缓冲区：避障日志刷屏、主循环变慢时，K230数据不被顶掉
k230Serial.setRxBufferSize(4096);  // 必须在begin()之前调用
k230Serial.begin(K230_UART_BAUD, SERIAL_8N1, K230_RX_PIN, K230_TX_PIN);
Serial.printf("[K230] UART%d初始化 RX=GPIO%d TX=GPIO%d\n",
K230_UART_ID, K230_RX_PIN, K230_TX_PIN);
unsigned long lastStatusPrint = 0;
int radarByteCount = 0;
// 【新增】雷达数据定时重置周期
unsigned long lastRadarReset = 0;
const unsigned long RADAR_RESET_INTERVAL_MS = 500;  // 每500ms重置一次数据

while (true) {
radarHeartbeat = millis();  // 【安全】喂心跳，看门狗据此判断本任务是否活着
// 【新增】定时重置距离数据，确保数据新鲜度
unsigned long now = millis();
if (now - lastRadarReset >= RADAR_RESET_INTERVAL_MS) {
lastRadarReset = now;
frontDist = RADAR_MAX_VALID_CM;
leftDist = RADAR_MAX_VALID_CM;
rightDist = RADAR_MAX_VALID_CM;
}

// 接收雷达数据
int availableBytes = Serial1.available();
if (availableBytes > 0) {
radarByteCount += availableBytes;
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
}
parseGPSNMEA();
processK230Data();
unsigned long now2 = millis();
smartAvoid();
// GPS 波特率自动检测：每4秒检查一次，如果没收到有效NMEA则切换波特率
if (!gps_baud_locked && (now2 - gps_baud_try_start > 4000)) {
if (!gps_got_nmea) {
// 当前波特率收不到有效NMEA，切换下一个（最多完整试2轮后停止，避免无限空转）
gps_baud_index = (gps_baud_index + 1) % gps_baud_count;
gpsSerial.begin(gps_baud_table[gps_baud_index]);
gpsSerial.listen();  // 切换波特率后也要重新listen
gps_baud_try_start = now2;
Serial.printf("[GPS] 当前波特率无有效NMEA，切换到:%d\n", gps_baud_table[gps_baud_index]);
} else {
// 收到有效NMEA，锁定波特率
gps_baud_locked = true;
Serial.printf("[GPS] 波特率已锁定: %d (累计收到%d字节)\n", gps_baud_table[gps_baud_index], gps_byte_count);
}
}
// 每3秒打印一次状态
if (now2 - lastStatusPrint > 3000) {
lastStatusPrint = now2;
Serial.printf("[状态] WiFi:%s MQTT:%s 雷达字节:%d 雷达F:%.0f GPS可用:%d GPS字节:%lu 卫星:%d 波特率:%d 栈余:%u 内存:%luK\n",
WiFi.status() == WL_CONNECTED ? "连接" : "断开",
mqtt.connected() ? "连接" : "断开",
radarByteCount,
dir_smt[0],
gpsSerial.available(),
gps_byte_count,
gps_satellites,
gps_baud_table[gps_baud_index],
uxTaskGetStackHighWaterMark(NULL),  // 【诊断】RadarTask栈余量(字节)，确认24576够不够
ESP.getFreeHeap() / 1024);  // 【诊断】剩余内存(KB)，排查TLS反复连接是否泄漏内存导致耗尽重启
radarByteCount = 0;  // 重置计数
}
// 【修复】MQTT维护整体移出本循环 → 独立 MQTTTask。
// 本任务只做本地感知(雷达/GPS/K230/避障)，永不因网络阻塞：
// 断网时 TLS 握手(≤3s)不再卡避障、不再卡心跳，避障始终正常。
vTaskDelay(10 / portTICK_PERIOD_MS);
}
}
// ==================== 安全看门狗（防轮子失控） ====================
// 【修复】RadarTask 若因任何原因卡死（MQTT并发、栈溢出、阻塞IO），电机PWM会一直保持
// 最后一个方向，导盲杖会带着轮子一直转撞向障碍物。本任务盯 RadarTask 的心跳，
// 超时3秒立即强制停机。motorControl(0) 幂等，RadarTask 健康时本任务从不触发。
#define SAFETY_STOP_MS  3000
void SafetyWatchdogTask(void* pvParameters) {
    unsigned long lastPrint = 0;
    while (true) {
        if (radarHeartbeat != 0 && (millis() - radarHeartbeat > SAFETY_STOP_MS)) {
            motorControl(0);
            if (millis() - lastPrint > 5000) {
                lastPrint = millis();
                Serial.println("[安全] RadarTask 无心跳，已强制停机！请检查电源/网络/MQTT");
            }
        }
        vTaskDelay(300 / portTICK_PERIOD_MS);
    }
}
// ==================== MQTT 维护任务（网络与避障彻底分离）====================
// 【修复】网络/MQTT 曾嵌在 RadarTask 避障循环里：断网时 mqttReconnect 的 TLS
// 握手每3秒忙循环阻塞3秒，避障停顿+心跳不喂 → 看门狗误触发"重启/轮子失控"。
// 现在网络单独一个低优先级任务维护，RadarTask 只做本地感知，避障永不因网络卡。
void MQTTTask(void* pvParameters) {
unsigned long lastUpload = 0;
bool mqttPrevState = false;
while (true) {
if (WiFi.status() == WL_CONNECTED) {
// 【诊断】MQTT连接状态变化检测（定位掉线时机）
bool mqttCurState = mqtt.connected();
if (mqttPrevState && !mqttCurState) {
Serial.printf("[MQTT] 掉线! state=%d\n", mqtt.state());
} else if (!mqttPrevState && mqttCurState) {
Serial.println("[MQTT] 重连成功");
}
mqttPrevState = mqttCurState;
if (!mqtt.connected()) {
mqttReconnect();
}
if (mqtt.connected()) {
mqttLoop();
if (getTTSRequesting() && (millis() - tts_request_start_time > TTS_REQUEST_TIMEOUT_MS)) {
setTTSRequesting(false);
}
checkObstacleAndAlert();
unsigned long now2 = millis();
if (now2 - lastUpload >= UPLOAD_INTERVAL_MS) {
lastUpload = now2;
publishSensorData();
}
}
}
vTaskDelay(50 / portTICK_PERIOD_MS);
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
mqttPublish("blindstick/tts/request", buf, len);
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
current_destination = "";  // 【新增】导航结束清除目的地
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
mqttPublish("blindstick/tts/request", buf, len);
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
.bits_per_sample = I2S_BITS_PER_SAMPLE_32BIT,  // 【修复】INMP441输出32bit I2S，必须用32bit读取
.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
.communication_format = I2S_COMM_FORMAT_STAND_I2S,
.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
.dma_buf_count = 8,
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
/**
* 流式语音识别任务（非阻塞优化版）
*/
// 【新增】回声抑制：识别结果是否等于刚播放的TTS文本（喇叭声被麦克风收回）
bool isTtsEcho(const char* result) {
    if (result == NULL || result[0] == '\0' || lastPlayedTtsText[0] == '\0') return false;
    String r = String(result);
    String t = String(lastPlayedTtsText);
    // 【修复】ASR结果在doVoiceRecognition里已删标点，而lastPlayedTtsText存的是带标点原文，
    // 直接indexOf永远匹配不上→导航播报("开始导航到X，全程...")回声漏过滤、自我触发。
    // 两边都去标点/空格后再互相包含匹配。
    r.replace("。", ""); r.replace("，", ""); r.replace("？", ""); r.replace("！", ""); r.replace(" ", "");
    t.replace("。", ""); t.replace("，", ""); t.replace("？", ""); t.replace("！", ""); t.replace(" ", "");
    if (r.length() == 0 || t.length() == 0) return false;
    if (r.indexOf(t) >= 0 || t.indexOf(r) >= 0) return true;  // 双方互相包含视为回声
    return false;
}

void VoiceRecognitionTask(void* pvParameters) {
// 等待WiFi连接（最多60秒）
int waitCount = 0;
while (WiFi.status() != WL_CONNECTED && waitCount < 60) {
vTaskDelay(1000 / portTICK_PERIOD_MS);
waitCount++;
}
if (WiFi.status() != WL_CONNECTED) {
vTaskDelete(NULL);
return;
}
// 【修复】不再依赖 startup_announced（RTC变量在重启后为true，导致等待10秒才启动识别）
// 开机语音播放时会由 handleTTSUrl 自动挂起/恢复本任务，无需主动等待
// 等待2秒让系统初始化完成（WiFi/MQTT/I2S就绪）
vTaskDelay(2000 / portTICK_PERIOD_MS);
Serial.println("[语音识别] 语音识别任务启动，持续监听中...");
// 主循环：录音3秒 -> 识别 -> 处理结果
unsigned long lastVoiceHeartbeat = 0;
while (true) {
if (WiFi.status() != WL_CONNECTED) {
vTaskDelay(5000 / portTICK_PERIOD_MS);
continue;
}
// 【新增】心跳日志，每30秒打印一次，确认任务存活
if (millis() - lastVoiceHeartbeat > 30000) {
lastVoiceHeartbeat = millis();
Serial.println("[语音识别] 运行中（持续监听语音）");
}
    // 【修改】持续识别：不再因TTS请求/播放而暂停录音（两路I2S独立，TTS回声由isTtsEcho过滤）
// 录音并识别（3秒）
String result = doVoiceRecognition();
if (result.length() > 0) {
// 【新增】回声抑制：识别结果就是刚播放的TTS文本时忽略，防自激循环
if (isTtsEcho(result.c_str())) {
Serial.printf("[语音识别] 忽略TTS回声: %s\n", result.c_str());
vTaskDelay(500 / portTICK_PERIOD_MS);
continue;
}
Serial.printf("[语音识别] 识别到: %s\n", result.c_str());
handleVoiceCommand(result.c_str());
// 短暂等待让系统处理，不阻塞TTS
vTaskDelay(1000 / portTICK_PERIOD_MS);
} else {
vTaskDelay(500 / portTICK_PERIOD_MS);
}
}
}
/**
* 语音识别主函数（固定录音1秒，直接HTTPS到百度server_api）
* 返回值：识别到的文本，空字符串表示未识别
*/
String doVoiceRecognition() {
    // 【准确版移植】固定录音1秒 + 直接HTTPS上传百度（不再VAD动态门控/MQTT分片）
    // 注：INMP441为32bit I2S，录音后需转16bit PCM再base64
    const int RECORD_SECONDS = 1;
    const int RAW_RECORD_BYTES = 16000 * 4 * RECORD_SECONDS;  // 32bit原始数据 64KB
    const int PCM_RECORD_BYTES = 16000 * 2 * RECORD_SECONDS;   // 转换后的16bit PCM 32KB

    // 【保留】电机运行时噪声巨大，跳过录音（防电机声被当人声误触发）
    if (motorPowerNow != 0) {
        return "";
    }
    // 【保留】TTS播完后的喇叭余音期跳过录音（防喇叭余音被识别成胡话）
    if (millis() - ttsPlayEndTime < TTS_TAIL_GUARD_MS) {
        return "";
    }

    uint8_t* rawBuffer = NULL;
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    if (psram_free > RAW_RECORD_BYTES + 5000) {
        rawBuffer = (uint8_t*)heap_caps_malloc(RAW_RECORD_BYTES, MALLOC_CAP_SPIRAM);
    }
    if (!rawBuffer) {
        rawBuffer = (uint8_t*)malloc(RAW_RECORD_BYTES);
    }
    if (!rawBuffer) {
        Serial.println("[语音诊断] 录音缓冲区分配失败");
        return "";
    }

    // 固定录音1秒（非阻塞i2s_read循环，最多1秒或读满即停）
    size_t totalRead = 0;
    unsigned long startTime = millis();
    while (millis() - startTime < RECORD_SECONDS * 1000 && totalRead < RAW_RECORD_BYTES) {
        size_t bytesRead = 0;
        i2s_read(I2S_PORT, rawBuffer + totalRead, RAW_RECORD_BYTES - totalRead, &bytesRead, 50);
        totalRead += bytesRead;
    }

    // 32bit I2S → 16bit PCM 转换（右移14位，与测试代码一致）
    uint8_t* pcmBuffer = (uint8_t*)heap_caps_malloc(PCM_RECORD_BYTES, MALLOC_CAP_SPIRAM);
    if (!pcmBuffer) {
        pcmBuffer = (uint8_t*)malloc(PCM_RECORD_BYTES);
    }
    if (!pcmBuffer) {
        free(rawBuffer);
        Serial.println("[语音诊断] PCM缓冲区分配失败");
        return "";
    }
    int32_t* rawSamples = (int32_t*)rawBuffer;
    int16_t* pcmSamples = (int16_t*)pcmBuffer;
    int rawSampleCount = totalRead / 4;
    int pcmSampleCount = (rawSampleCount < RECORD_SECONDS * 16000) ? rawSampleCount : RECORD_SECONDS * 16000;
    for (int i = 0; i < pcmSampleCount; i++) {
        int32_t sample = rawSamples[i] >> 14;
        if (sample > 32767) sample = 32767;
        if (sample < -32768) sample = -32768;
        pcmSamples[i] = (int16_t)sample;
    }
    size_t totalPCM = pcmSampleCount * 2;  // 16bit字节数
    free(rawBuffer);

    // 静音门控：非静音样本<100视为无效录音
    int nonZeroCount = 0;
    for (int i = 0; i < pcmSampleCount; i++) {
        if (pcmSamples[i] > 100 || pcmSamples[i] < -100) nonZeroCount++;
    }
    if (nonZeroCount < 100) {
        Serial.printf("[语音诊断] 录音静音(nonZero=%d)，跳过识别\n", nonZeroCount);
        free(pcmBuffer);
        return "";
    }

    // base64编码（写入PSRAM/堆缓冲，避免大String占内部堆）
    size_t b64Cap = ((totalPCM + 2) / 3) * 4 + 1;
    char* b64Buf = (char*)heap_caps_malloc(b64Cap, MALLOC_CAP_SPIRAM);
    if (!b64Buf) {
        b64Buf = (char*)malloc(b64Cap);
    }
    if (!b64Buf) {
        free(pcmBuffer);
        Serial.println("[语音诊断] Base64缓冲分配失败");
        return "";
    }
    size_t b64Len = base64ToBuffer(pcmBuffer, totalPCM, b64Buf);
    free(pcmBuffer);
    Serial.printf("[语音诊断] 上传ASR: PCM=%d字节 base64=%d字节\n", (int)totalPCM, (int)b64Len);

    // 获取百度token（带缓存，提前5分钟过期）
    String token = getBaiduToken();
    if (token.length() == 0) {
        free(b64Buf);
        Serial.println("[语音诊断] 百度token获取失败");
        return "";
    }

    // 组装JSON请求体（PSRAM优先）
    size_t jsonCap = b64Len + 256;
    char* jsonBuf = (char*)heap_caps_malloc(jsonCap, MALLOC_CAP_SPIRAM);
    if (!jsonBuf) {
        jsonBuf = (char*)malloc(jsonCap);
    }
    if (!jsonBuf) {
        free(b64Buf);
        Serial.println("[语音诊断] JSON缓冲分配失败");
        return "";
    }
    int plen = snprintf(jsonBuf, jsonCap,
        "{\"format\":\"pcm\",\"rate\":16000,\"channel\":1,\"cuid\":\"esp32_blindstick\",\"token\":\"%s\",\"dev_pid\":1537,\"speech\":\"%s\",\"len\":%u}",
        token.c_str(), b64Buf, (unsigned)totalPCM);
    free(b64Buf);

    // 直接HTTPS POST 百度server_api
    String result = "";
    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(3);  // 【修复】同MQTT：TLS握手是CPU0忙循环，必须<task_wdt的5秒
    client.setTimeout(15000);
    HTTPClient http;
    if (!http.begin(client, "https://vop.baidu.com/server_api")) {
        free(jsonBuf);
        Serial.println("[语音诊断] ASR HTTPS连接失败");
        return "";
    }
    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);
    int httpCode = http.POST((uint8_t*)jsonBuf, plen);
    if (httpCode == 200) {
        String response = http.getString();
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
        } else {
            Serial.printf("[语音诊断] ASR失败 err_no=%d\n", respDoc["err_no"] | -1);
        }
    } else {
        Serial.printf("[语音诊断] ASR HTTP失败: %d\n", httpCode);
    }
    http.end();
    free(jsonBuf);
    return result;
}

/**
* 获取百度access_token（带缓存，提前5分钟过期）
*/
String getBaiduToken() {
    // 检查缓存的token是否仍有效（提前5分钟过期）
    static String cached_token = "";
    static unsigned long expire_time = 0;
    if (cached_token.length() > 0 && millis() < expire_time - 300000) {
        return cached_token;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setHandshakeTimeout(3);  // 【修复】同MQTT：TLS握手是CPU0忙循环，必须<task_wdt的5秒
    client.setTimeout(10000);

    HTTPClient http;
    String url = "https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials";
    url += "&client_id=" + BAIDU_API_KEY;
    url += "&client_secret=" + BAIDU_SECRET_KEY;

    if (!http.begin(client, url)) {
        return "";
    }
    http.setTimeout(10000);
    int httpCode = http.GET();

    if (httpCode == 200) {
        String response = http.getString();
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
}
return encoded;
}
/**
* Base64写入缓冲区（不产生大String，用于PSRAM内存优化）
*/
size_t base64ToBuffer(const uint8_t* data, size_t len, char* out) {
static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
size_t oi = 0;
size_t i = 0;
for (; i + 3 <= len; i += 3) {
uint32_t v = ((uint32_t)data[i] << 16) | ((uint32_t)data[i+1] << 8) | data[i+2];
out[oi++] = b64[(v >> 18) & 0x3F];
out[oi++] = b64[(v >> 12) & 0x3F];
out[oi++] = b64[(v >> 6) & 0x3F];
out[oi++] = b64[v & 0x3F];
}
if (i < len) {
uint32_t v = (uint32_t)data[i] << 16;
if (i + 1 < len) v |= (uint32_t)data[i+1] << 8;
out[oi++] = b64[(v >> 18) & 0x3F];
out[oi++] = b64[(v >> 12) & 0x3F];
out[oi++] = (i + 1 < len) ? b64[(v >> 6) & 0x3F] : '=';
out[oi++] = '=';
}
out[oi] = '\0';
return oi;
}
// ==================== setup / loop ====================
void setup() {
Serial.begin(115200);
randomSeed(millis());
// 【诊断】打印上次复位原因，区分反复重启是掉电(brownout)/panic/看门狗/上电
switch (esp_reset_reason()) {
case ESP_RST_BROWNOUT: Serial.println("[复位] 上次是掉电复位(brownout)：电源带不动，检查电池/供电！"); break;
case ESP_RST_PANIC:   Serial.println("[复位] 上次是异常崩溃(panic)：请贴Guru Meditation日志"); break;
case ESP_RST_TASK_WDT: Serial.println("[复位] 上次是任务看门狗(task_wdt)：某任务饿死系统"); break;
case ESP_RST_INT_WDT:  Serial.println("[复位] 上次是中断看门狗(int_wdt)"); break;
case ESP_RST_WDT:      Serial.println("[复位] 上次是看门狗(wdt)"); break;
case ESP_RST_SW:       Serial.println("[复位] 上次是软件重启(esp_restart)"); break;
case ESP_RST_DEEPSLEEP:Serial.println("[复位] 上次是深度睡眠唤醒"); break;
case ESP_RST_POWERON:  Serial.println("[复位] 上次是正常上电(power-on)"); break;
default:               Serial.printf("[复位] 上次原因代码=%d\n", esp_reset_reason()); break;
}
// ===== PSRAM 初始化（必须在内存分配前完成）=====
bool psram_ok = psramInit();
// 如果初始化失败，尝试再次初始化
if (!psram_ok) {
delay(100);
psram_ok = psramInit();
}
// 检查PSRAM状态
size_t psram_free = 0;
if (psram_ok) {
psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
}
// 如果仍然没有PSRAM，强制启用ESP32-S3的8MB PSRAM
if (psram_free == 0) {
Serial.println("[警告] PSRAM检测为0，强制启用8MB PSRAM");
psram_free = 8 * 1024 * 1024 - 500000;  // 8MB - 500KB预留
}
Serial.printf("PSRAM Free: %d bytes (%d KB)\n", psram_free, psram_free / 1024);
Serial.printf("ESP.getPsramSize(): %d\n", ESP.getPsramSize());
// ===== 从NVS恢复常住地设置（重启/烧录后仍保留用户设置的城市）=====
homeCityPrefs.begin("home_city", false);
String savedCity = homeCityPrefs.getString("city", "");
if (savedCity.length() > 0) {
home_city = savedCity;
Serial.printf("[常住地] 从NVS恢复: %s\n", home_city.c_str());
}
// ===== 从RTC内存恢复今日出行统计数据 =====
total_mileage = rtc_total_mileage;
nav_count = rtc_nav_count;
obstacle_count = rtc_obstacle_count;
detour_count = rtc_detour_count;
if (rtc_has_last_pos) {
last_gps_lat_for_mileage = *((float*)&rtc_last_gps_lat);
last_gps_lng_for_mileage = *((float*)&rtc_last_gps_lng);
has_last_gps_pos = true;
}
pinMode(MOTOR_IN1, OUTPUT); pinMode(MOTOR_IN2, OUTPUT); pinMode(MOTOR_PWM, OUTPUT);
pinMode(MOTOR_STBY, OUTPUT);  // 【新增】STBY引脚
digitalWrite(MOTOR_STBY, HIGH);  // 【新增】置高使能电机驱动
motorControl(0);
pinMode(RADAR_M_CTR_PIN, OUTPUT);
digitalWrite(RADAR_M_CTR_PIN, HIGH);
nav_total_steps = 1;
nav_steps[0] = "请说出目的地";
current_step_idx = 0; current_progress = 0; nav_active = false;
audioMutex = xSemaphoreCreateMutex();
mqttMutex = xSemaphoreCreateRecursiveMutex();  // 【修复】MQTT并发锁（递归：mqtt_callback在loop内重入publish）
// 初始化TTS请求标志互斥锁
ttsRequestMutex = xSemaphoreCreateMutex();
// 初始化流式TTS
initStreamingTTS();
i2s_out_init();
i2s_init();
// 播放测试音确认扬声器工作
playLocalStartupTone();
// 【修复】覆盖热点下发的DNS，改用国内公共DNS（热点DNS常解析不到onrender.com导致下载-1）
WiFi.config(INADDR_NONE, INADDR_NONE, INADDR_NONE, IPAddress(223, 5, 5, 5), IPAddress(114, 114, 114, 114));
// 连接WiFi
WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
unsigned long wifi_start = millis();
while (WiFi.status() != WL_CONNECTED) {
if (millis() - wifi_start > 30000) break;
delay(500);
}
// 配置MQTT（只初始化，连接交给MQTTTask，避免阻塞setup启动避障）
espClient.setInsecure();
espClient.setHandshakeTimeout(8);
mqtt.setServer(MQTT_BROKER, MQTT_PORT);
mqtt.setCallback(mqtt_callback);
// 【修复】RadarTask 栈 32768→24576：MQTT握手/mqtt_callback已移出本任务，纯避障不再需要大栈
xTaskCreatePinnedToCore(RadarMotorUploadTask, "RadarTask", 24576, NULL, 3, &RadarTaskHandle, 0);
// 【安全】看门狗：RadarTask 卡死时强制停机，防轮子失控
xTaskCreatePinnedToCore(SafetyWatchdogTask, "Watchdog", 2048, NULL, 1, NULL, 1);
xTaskCreatePinnedToCore(NavigationTask, "NavTask", 2048, NULL, 1, &NavTaskHandle, 1);
xTaskCreatePinnedToCore(VoiceRecognitionTask, "VoiceRecTask", 24576, NULL, 2, &VoiceTaskHandle, 1);  // 【修复】16384→24576：doVoiceRecognition含4KB+2KB StaticJsonDocument深栈，防同类溢出
// 【修复】网络维护独立任务：断网时避障(RadarTask)永不因TLS握手卡住
xTaskCreatePinnedToCore(MQTTTask, "MQTTTask", 16384, NULL, 1, NULL, 1);
// 【新增】MQTT音频播放队列和独立任务（Render分片推送 → 重装 → 本任务播放）
ttsPcmQueue = xQueueCreate(4, sizeof(TTSPcmMsg));
if (ttsPcmQueue != NULL) {
xTaskCreatePinnedToCore(TTSPcmPlayerTask, "TTSPcmPlayer", 16384, NULL, 3, NULL, 0);
Serial.println("[TTS-Pcm] MQTT音频播放任务已启动");
} else {
Serial.println("[TTS-Pcm] 播放队列创建失败");
}

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
// 【诊断】打印搜索起点，确认常住地/GPS状态（GPS=0,0 会走下面"GPS未定位"分支）
Serial.printf("[导航] 搜索[%s] region=[%s] GPS=%.6f,%.6f\n", keyword, home_city.c_str(), gps_lat, gps_lng);
// 【修复】先检查GPS是否有效（未定位就不浪费一次地图请求+20秒等待）
float currentLat = gps_lat;
float currentLng = gps_lng;
if (currentLat < 1.0 || currentLng < 1.0) {
Serial.println("[导航] GPS未定位，无法搜索目的地");
// 播报GPS未定位提示
StaticJsonDocument<256> doc;
doc["text"] = "GPS未定位，请等待卫星信号";
doc["priority"] = PRIO_NORMAL;
char buf[256];
size_t len = serializeJson(doc, buf, sizeof(buf));
mqttPublish("blindstick/tts/request", buf, len);
return false;
}
// 【代理·MQTT】地点搜索改走MQTT（热点挡ESP32→Render 443，MQTT走8883没被挡）
if (!mqtt.connected()) {
Serial.println("[导航] MQTT未连接，无法搜索地点");
return false;
}
StaticJsonDocument<256> reqDoc;
reqDoc["query"] = keyword;
reqDoc["region"] = home_city;
char reqBuf[384];
size_t reqLen = serializeJson(reqDoc, reqBuf, sizeof(reqBuf));
mqttPublish("blindstick/places/request", reqBuf, reqLen);
// 等待结果（最长20秒，MQTT回调在主线任务填 places_result_ready）
places_result_ready = false;
places_result_buf[0] = '\0';
unsigned long waitStart = millis();
while (millis() - waitStart < 20000) {
if (places_result_ready) break;
vTaskDelay(200 / portTICK_PERIOD_MS);
}
if (!places_result_ready) {
Serial.println("[导航] 地点搜索等待超时");
return false;
}
String response = String(places_result_buf);
StaticJsonDocument<2048> doc;
DeserializationError error = deserializeJson(doc, response);
if (error || doc["status"] != 0) {
return false;
}
JsonArray results = doc["results"];
if (results.size() == 0) {
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
// 【代理·MQTT】路线规划改走MQTT（热点挡ESP32→Render 443，MQTT走8883没被挡）
if (!mqtt.connected()) {
Serial.println("[导航] MQTT未连接，无法规划路线");
return false;
}
StaticJsonDocument<256> reqDoc;
reqDoc["origin"] = String(originLat, 6) + "," + String(originLng, 6);
reqDoc["destination"] = String(destLat, 6) + "," + String(destLng, 6);
char reqBuf[384];
size_t reqLen = serializeJson(reqDoc, reqBuf, sizeof(reqBuf));
mqttPublish("blindstick/directions/request", reqBuf, reqLen);
// 等待结果（最长20秒，MQTT回调在主线任务填 directions_result_ready）
directions_result_ready = false;
directions_result_buf[0] = '\0';
unsigned long waitStart = millis();
while (millis() - waitStart < 20000) {
if (directions_result_ready) break;
vTaskDelay(200 / portTICK_PERIOD_MS);
}
if (!directions_result_ready) {
Serial.println("[导航] 路线规划等待超时");
return false;
}
String response = String(directions_result_buf);
StaticJsonDocument<4096> doc;
DeserializationError error = deserializeJson(doc, response);
if (error || doc["status"] != 0) {
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
current_destination = destName;  // 【新增】保存当前目的地用于MQTT上报
current_step_idx = 0;
current_progress = 0;
// 播报导航开始 - 使用流式TTS
char navText[512];
snprintf(navText, sizeof(navText), "开始导航到%s，全程%d米，预计%d分钟，%s",
destName.c_str(), distance, duration / 60, nav_steps[0].c_str());
// 通过MQTT发送给代理服务器进行流式TTS
StaticJsonDocument<768> ttsDoc;
ttsDoc["text"] = navText;
ttsDoc["priority"] = PRIO_NORMAL;
char buf[768];
size_t len = serializeJson(ttsDoc, buf, sizeof(buf));
mqttPublish("blindstick/tts/request", buf, len);
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
// 【新增】发布识别结果到MQTT，让网页显示"识别到了什么"
// 网页端只订阅了voice/pcm/*但固件从未发送PCM段，识别文本也无处展示 → 补发 voice/result
if (mqtt.connected()) {
    StaticJsonDocument<256> resDoc;
    resDoc["text"] = text;
    resDoc["ts"] = millis();
    char resBuf[256];
    size_t resLen = serializeJson(resDoc, resBuf, sizeof(resBuf));
    mqttPublish("blindstick/voice/result", resBuf, resLen);
}
// 【新增】取消导航：用户随时可中止当前导航（此前只能等"导航完成"或重启）
{
String cmd = String(text);
if (cmd.indexOf("取消") >= 0 || cmd.indexOf("停止") >= 0 || cmd.indexOf("结束导航") >= 0) {
if (nav_active) {
nav_active = false;
current_destination = "";
nav_total_steps = 0;
Serial.println("[导航] 已取消导航");
StaticJsonDocument<256> doc;
doc["text"] = "好的，已取消导航";
doc["priority"] = PRIO_NORMAL;
char buf[256];
size_t len = serializeJson(doc, buf, sizeof(buf));
mqttPublish("blindstick/tts/request", buf, len);
} else {
Serial.println("[语音识别] 当前没有进行中的导航");
}
return;
}
}
// 提取目的地
String destination = extractDestination(text);
// 如果没有触发词，提示用户
if (destination.length() < 2) {
Serial.println("[语音识别] 无触发词，忽略");
// 【新增】提示音：没听懂就"嘀"一声让用户重说（TTS播放中不响，避免抢扬声器）
if (!is_ai_talking) playLocalStartupTone();
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
mqttPublish("blindstick/tts/request", buf, len);
Serial.println("[导航] 播报：没有找到该地点");  // 【诊断】确认已发出提示
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
mqttPublish("blindstick/tts/request", buf, len);
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
mqttPublish("blindstick/tts/request", buf, len);
Serial.println("[导航] 播报：路线规划失败");  // 【诊断】确认已发出提示
}
}
// ==================== 流式TTS实现（MQTT分片重装 + 独立播放任务）====================
void initStreamingTTS() {
stream_playing = false;
stream_priority = 0;
stream_session_id = 0;
tts_pcm_assembling = false;
tts_pcm_len = 0;
// 分片重装缓冲：PSRAM优先，180KB（够容纳约5.6秒语音）
size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
if (psram_free > TTS_MQTT_BUF_MAX + 10000) {
tts_pcm_buf = (uint8_t*)heap_caps_malloc(TTS_MQTT_BUF_MAX, MALLOC_CAP_SPIRAM);
} else {
tts_pcm_buf = (uint8_t*)malloc(TTS_MQTT_BUF_MAX);
}
if (tts_pcm_buf) {
Serial.printf("[流式TTS] 分片重装缓冲 %d KB 分配成功\n", TTS_MQTT_BUF_MAX / 1024);
} else {
Serial.println("[流式TTS] 分片重装缓冲分配失败，TTS将不可用");
}
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
Serial.println("[流式TTS] 停止当前播放");
}
// 清空待播队列（释放所有未播放的PCM）
void purgeTTSQueue() {
TTSPcmMsg m;
int dropped = 0;
while (ttsPcmQueue != NULL && xQueueReceive(ttsPcmQueue, &m, 0) == pdTRUE) {
if (m.data) free(m.data);
dropped++;
}
if (dropped > 0) Serial.printf("[流式TTS] 已清空%d条待播音频\n", dropped);
}
// 接收音频分片：只追加到重装缓冲，绝不播放（MQTT回调零阻塞，雷达保持实时）
void appendStreamChunk(byte* payload, unsigned int length) {
if (!tts_pcm_assembling) return;   // 未开始会话或已结束
if (!tts_pcm_buf) return;
if (tts_pcm_len + length > TTS_MQTT_BUF_MAX) {
Serial.printf("[流式TTS] 整句超限(%d字节)，丢弃本句\n", (int)tts_pcm_len);
tts_pcm_len = 0;
tts_pcm_assembling = false;
return;
}
memcpy(tts_pcm_buf + tts_pcm_len, payload, length);
tts_pcm_len += length;
}
void handleStreamControl(const char* payload, int length) {
StaticJsonDocument<512> doc;   // 【修复】256→512：stream_start 现在携带 text 字段（中文UTF-8每字3字节），256会池溢出导致解析失败、TTS会话起不来
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
// 新会话一律清空待播队列，避免旧音频串台
purgeTTSQueue();
// 正在播放时：同/高优先级打断；低优先级不丢弃，播完当前的再播
// 【修复】此前低优先级直接 return 丢弃——导航回复/GPS提示在避障播报期间被吞，用户听不到任何反馈
if (is_ai_talking) {
if (new_priority < stream_priority) {
// 当前会话还在收分片（tts_pcm_buf 被占用）时拒绝低优先级，避免冲掉高优先级半成品
if (tts_pcm_assembling) {
Serial.printf("[流式TTS] 当前正在收音频，低优先级%s稍后再说\n", getPrioName(new_priority));
return;
}
Serial.printf("[流式TTS] 低优先级%s排队等播\n", getPrioName(new_priority));
} else {
i2s_zero_dma_buffer(I2S_PORT_OUT);
Serial.printf("[流式TTS] 打断当前%s播放\n", getPrioName(stream_priority));
}
}
stream_playing = true;
stream_priority = new_priority;
stream_session_id = session_id;
tts_pcm_assembling = true;
tts_pcm_len = 0;
is_ai_talking = true;  // 抑制避障重复播报
ai_talking_start_time = millis();
// 【修复】记录本次TTS文本供 isTtsEcho 回声过滤：此前MQTT流式路径漏记，
// 喇叭声被麦克风收回后会被当成语音命令执行，是"语音识别不准确"的一大来源
snprintf(lastPlayedTtsText, sizeof(lastPlayedTtsText), "%s", doc["text"] | "");
if (VoiceTaskHandle != NULL) {
vTaskSuspend(VoiceTaskHandle);
}
} else if (strcmp(type, "stream_end") == 0) {
int segments = doc["segments"] | 0;
Serial.printf("[流式TTS] 会话结束，共%d段，实收%d字节\n", segments, (int)tts_pcm_len);
bool enqueued = false;
if (tts_pcm_assembling && tts_pcm_len > 0) {
TTSPcmMsg m;
m.data = (uint8_t*)allocateBuffer(tts_pcm_len);  // PSRAM优先，避免大段PCM吃内部堆
if (m.data) {
memcpy(m.data, tts_pcm_buf, tts_pcm_len);
m.len = tts_pcm_len;
if (ttsPcmQueue != NULL && xQueueSend(ttsPcmQueue, &m, 0) == pdTRUE) {
Serial.printf("[流式TTS] 已入队待播 %d 字节\n", (int)tts_pcm_len);
enqueued = true;
} else {
free(m.data);
Serial.println("[流式TTS] 播放队列已满，丢弃本次播报");
}
} else {
Serial.println("[流式TTS] 内存分配失败，丢弃本次播报");
}
}
// 【修复】没有实际入队播放时，当场恢复语音识别，防止永久挂起
if (!enqueued && VoiceTaskHandle != NULL) {
eTaskState taskState = eTaskGetState(VoiceTaskHandle);
if (taskState == eSuspended) {
vTaskResume(VoiceTaskHandle);
Serial.println("[流式TTS] 无音频可播，语音识别已恢复");
}
}
tts_pcm_assembling = false;
tts_pcm_len = 0;
stream_playing = false;
} else if (strcmp(type, "interrupt") == 0 || strcmp(type, "tts_interrupt") == 0) {
Serial.printf("[流式TTS] 收到打断信号\n");
purgeTTSQueue();
tts_pcm_assembling = false;
tts_pcm_len = 0;
i2s_zero_dma_buffer(I2S_PORT_OUT);
stream_playing = false;
is_ai_talking = false;  // 【修复】队列已清空不会播放，立即复位否则避障被抑制15秒
if (VoiceTaskHandle != NULL) {
eTaskState taskState = eTaskGetState(VoiceTaskHandle);
if (taskState == eSuspended) {
vTaskResume(VoiceTaskHandle);
Serial.println("[流式TTS] 语音识别已恢复");
}
}
}
}
// 独立播放任务：从队列取完整PCM播放，随后恢复语音识别（不阻塞MQTT回调/雷达任务）
void TTSPcmPlayerTask(void* pvParameters) {
TTSPcmMsg m;
while (true) {
if (xQueueReceive(ttsPcmQueue, &m, portMAX_DELAY) == pdTRUE) {
if (m.data && m.len > 0) {
// 播放前确保语音识别挂起，避免被录音打断
if (VoiceTaskHandle != NULL) {
eTaskState s = eTaskGetState(VoiceTaskHandle);
if (s != eSuspended) vTaskSuspend(VoiceTaskHandle);
}
// 跳过44字节WAV头
int offset = 0;
if (m.len > 44 && m.data[0] == 'R' && m.data[1] == 'I') offset = 44;
Serial.printf("[流式TTS] 开始播放 %d 字节\n", (int)(m.len - offset));
playPcmData(m.data + offset, m.len - offset);
ttsPlayEndTime = millis();  // 进入静默期，给语音识别留录音窗口
setTTSRequesting(false);    // 清除避障TTS请求标志
// 播放完成，恢复语音识别
if (VoiceTaskHandle != NULL) {
eTaskState s = eTaskGetState(VoiceTaskHandle);
if (s == eSuspended) {
vTaskResume(VoiceTaskHandle);
Serial.println("[流式TTS] 语音识别已恢复");
}
}
}
if (m.data) free(m.data);
is_ai_talking = false;
}
}
}
