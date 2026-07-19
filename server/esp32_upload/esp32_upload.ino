#include <WiFi.h>
#include <PubSubClient.h>
#include <HardwareSerial.h>
#include <ArduinoJson.h>

// ==========================================
// WiFi & MQTT 配置
// ==========================================
const char* WIFI_SSID     = "ZZY";
const char* WIFI_PASSWORD = "zzy060630";
const char* MQTT_BROKER   = "u72a7838.ala.asia-southeast1.emqxsl.com";
const int   MQTT_PORT     = 8883;
const char* MQTT_USER     = "blindstick";
const char* MQTT_PASSWORD = "2026";
const char* MQTT_CLIENT_ID = "blindstick_esp32_001";

WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

// ==========================================
// 硬件引脚定义
// ==========================================
#define IN1             12
#define IN2             11
#define ENA             10
#define RADAR_RX_PIN    18
#define RADAR_M_CTR_PIN 8
#define GPS_RX_PIN      16
#define GPS_TX_PIN      17

HardwareSerial lidarSerial(1);
HardwareSerial gpsSerial(2);

// ==========================================
// 避障参数
// ==========================================
#define STEER_MAX_PWM   230
#define STEER_SLOW_PWM  180
#define FRONT_CRITICAL  60.0
#define SIDE_WARNING    50.0
#define ANG_FRONT_MIN   330
#define ANG_FRONT_MAX   30
#define ANG_LEFT_MIN    60
#define ANG_LEFT_MAX    120
#define ANG_RIGHT_MIN   240
#define ANG_RIGHT_MAX   300

float frontDist = 200.0, leftDist = 200.0, rightDist = 200.0;
float gps_lat = 0.0, gps_lng = 0.0;
int gps_sats = 0;

// 雷达状态机
enum LidarState { WAIT_HEADER_AA, WAIT_HEADER_55, READ_CT, READ_LSN, READ_PAYLOAD };
LidarState currentState = WAIT_HEADER_AA;
uint8_t packetCT = 0, packetLSN = 0, payloadBuffer[128], payloadIndex = 0, payloadExpected = 0;

// ==========================================
// 电机控制
// ==========================================
void controlOmniWheel(int steerPower) {
    int safePower = constrain(steerPower, -STEER_MAX_PWM, STEER_MAX_PWM);
    if (safePower > 15) {
        digitalWrite(IN1, HIGH); digitalWrite(IN2, LOW);
        analogWrite(ENA, safePower);
    } else if (safePower < -15) {
        digitalWrite(IN1, LOW); digitalWrite(IN2, HIGH);
        analogWrite(ENA, abs(safePower));
    } else {
        digitalWrite(IN1, LOW); digitalWrite(IN2, LOW);
        analogWrite(ENA, 0);
    }
}

// ==========================================
// 避障算法
// ==========================================
void smartAvoid() {
    float leftForce = 0.0, rightForce = 0.0;

    if (leftDist < SIDE_WARNING) leftForce = (SIDE_WARNING - leftDist) * 4.0;
    if (rightDist < SIDE_WARNING) rightForce = (SIDE_WARNING - rightDist) * 4.0;

    if (frontDist < FRONT_CRITICAL) {
        Serial.printf("🚨 前方紧急(%.1fcm)！\n", frontDist);
        if (leftDist > rightDist) {
            Serial.println("👈 全速向左闪避！");
            controlOmniWheel(-STEER_MAX_PWM);
        } else {
            Serial.println("👉 全速向右闪避！");
            controlOmniWheel(STEER_MAX_PWM);
        }
        return;
    }

    if (leftForce > 0 || rightForce > 0) {
        float netSteer = leftForce - rightForce;
        float scaleRatio = STEER_SLOW_PWM / (SIDE_WARNING * 4.0f);
        int slowSteer = netSteer * scaleRatio;
        Serial.printf("⚠️ 低速微调 -> 左:%.1f 右:%.1f | 输出:%d\n", leftDist, rightDist, slowSteer);
        controlOmniWheel(slowSteer);
    } else {
        controlOmniWheel(0);
    }
}

// ==========================================
// 雷达解析
// ==========================================
void processPacket() {
    uint16_t fsa = payloadBuffer[0] | (payloadBuffer[1] << 8);
    uint16_t lsa = payloadBuffer[2] | (payloadBuffer[3] << 8);
    float angleFSA = (fsa >> 1) / 64.0f;
    float angleLSA = (lsa >> 1) / 64.0f;
    float diffAngle = angleLSA - angleFSA;
    if (diffAngle < 0) diffAngle += 360.0f;

    frontDist = leftDist = rightDist = 200.0;

    for (int i = 0; i < packetLSN; i++) {
        uint16_t si = payloadBuffer[6 + i * 2] | (payloadBuffer[6 + i * 2 + 1] << 8);
        float distanceMm = si / 4.0f;
        if (distanceMm > 50.0f && distanceMm < 6000.0f) {
            float cm = distanceMm / 10.0f;
            float currentAngle = angleFSA + (packetLSN > 1 ? (diffAngle / (packetLSN - 1)) * i : 0);
            if (currentAngle >= 360.0f) currentAngle -= 360.0f;

            if (currentAngle >= ANG_FRONT_MIN || currentAngle <= ANG_FRONT_MAX) {
                if (cm < frontDist) frontDist = cm;
            } else if (currentAngle >= ANG_LEFT_MIN && currentAngle <= ANG_LEFT_MAX) {
                if (cm < leftDist) leftDist = cm;
            } else if (currentAngle >= ANG_RIGHT_MIN && currentAngle <= ANG_RIGHT_MAX) {
                if (cm < rightDist) rightDist = cm;
            }
        }
    }
}

// ==========================================
// GPS解析
// ==========================================
void parseGPS() {
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
                        gps_sats = sats;
                        if (fix > 0 && lat_raw > 0.0f && lng_raw > 0.0f) {
                            float lat = lat_raw / 100.0f;
                            lat = floor(lat) + (lat - floor(lat)) * 100.0f / 60.0f;
                            if (ns == 'S') lat = -lat;
                            float lng = lng_raw / 100.0f;
                            lng = floor(lng) + (lng - floor(lng)) * 100.0f / 60.0f;
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

// ==========================================
// MQTT上报
// ==========================================
void publishData() {
    if (!mqtt.connected()) return;
    StaticJsonDocument<256> doc;
    doc["device_id"] = "blindstick_001";
    JsonObject radar = doc.createNestedObject("radar");
    radar["f"] = frontDist;
    radar["l"] = leftDist;
    radar["r"] = rightDist;
    JsonObject gps = doc.createNestedObject("gps");
    gps["lat"] = gps_lat;
    gps["lng"] = gps_lng;
    gps["sats"] = gps_sats;
    char buf[256];
    size_t len = serializeJson(doc, buf);
    mqtt.publish("blindstick/sensors", buf, len);
}

void connectMQTT() {
    espClient.setInsecure();
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    while (!mqtt.connected()) {
        if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
            Serial.println("[MQTT] 已连接");
        } else {
            delay(5000);
        }
    }
}

// ==========================================
// Setup & Loop
// ==========================================
void setup() {
    Serial.begin(115200);
    pinMode(IN1, OUTPUT); pinMode(IN2, OUTPUT); pinMode(ENA, OUTPUT);
    controlOmniWheel(0);
    pinMode(RADAR_M_CTR_PIN, OUTPUT);
    for (int s = 0; s < 200; s += 10) { analogWrite(RADAR_M_CTR_PIN, s); delay(20); }

    lidarSerial.begin(115200, SERIAL_8N1, RADAR_RX_PIN, -1);
    gpsSerial.begin(9600, SERIAL_8N1, GPS_RX_PIN, GPS_TX_PIN);

    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    while (WiFi.status() != WL_CONNECTED) delay(500);
    Serial.println("[WiFi] 已连接");

    connectMQTT();
    Serial.println("🤖 系统启动完成");
}

void loop() {
    static unsigned long lastPub = 0;

    if (!mqtt.connected()) connectMQTT();
    mqtt.loop();

    // 雷达数据读取
    while (lidarSerial.available()) {
        uint8_t b = lidarSerial.read();
        switch (currentState) {
            case WAIT_HEADER_AA: if (b == 0xAA) currentState = WAIT_HEADER_55; break;
            case WAIT_HEADER_55: if (b == 0x55) currentState = READ_CT; else if (b != 0xAA) currentState = WAIT_HEADER_AA; break;
            case READ_CT: packetCT = b; currentState = READ_LSN; break;
            case READ_LSN: packetLSN = b; payloadExpected = 6 + packetLSN * 2; payloadIndex = 0; currentState = READ_PAYLOAD; break;
            case READ_PAYLOAD: payloadBuffer[payloadIndex++] = b; if (payloadIndex >= payloadExpected) { processPacket(); currentState = WAIT_HEADER_AA; } break;
        }
    }

    // GPS数据读取
    parseGPS();

    // 避障控制
    smartAvoid();

    // MQTT上报（每200ms）
    if (millis() - lastPub > 200) {
        publishData();
        lastPub = millis();
    }
}
