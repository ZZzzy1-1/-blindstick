/**
 * 录音回环测试程序
 *
 * 功能：
 * 1. 按 BOOT 键录音（2秒）
 * 2. 通过 MQTT 发送音频到后端
 * 3. 后端 ASR 识别 + TTS 合成
 * 4. 接收返回的语音并播放
 *
 * 硬件接线（与主程序一致）：
 * === INMP441 麦克风 ===
 * - VDD → 3.3V
 * - WS  → GPIO5 (LRCK)
 * - SCK → GPIO2 (BCLK)
 * - SD  → GPIO8 (MIC_IN)
 * - GND → GND
 * - L/R → GND
 *
 * === MAX98357 扬声器 ===
 * - BCLK → GPIO47 (SPK_BCLK)
 * - LRC  → GPIO41 (SPK_LRCK)
 * - DIN  → GPIO21 (SPK_OUT)
 * - GAIN → GND
 * - SD   → 3.3V
 * - GND  → GND
 * - VIN  → 3.3V
 */

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>
#include <driver/i2s.h>
#include <ArduinoJson.h>

// ==================== 配置 ====================
const char* WIFI_SSID     = "ZZY";
const char* WIFI_PASSWORD = "zzy060630";

// MQTT配置
const char* MQTT_BROKER   = "u72a7838.ala.asia-southeast1.emqxsl.com";
const int   MQTT_PORT     = 8883;
const char* MQTT_USER     = "blindstick";
const char* MQTT_PASSWORD = "2026";
const char* MQTT_CLIENT_ID = "blindstick_test_001";

// I2S引脚（与主程序一致）
#define I2S_WS_PIN      5   // LRCK
#define I2S_SCK_PIN     2   // BCLK
#define I2S_SD_PIN      8   // MIC_IN
#define I2S_PORT_MIC    I2S_NUM_0

#define I2S_BCK_PIN     47  // SPK_BCLK
#define I2S_WS_OUT_PIN  41  // SPK_LRCK
#define I2S_DATA_PIN    21  // SPK_OUT
#define I2S_PORT_OUT    I2S_NUM_1

#define SAMPLE_RATE     16000
#define VOLUME_GAIN     0.6

// 测试参数
#define TEST_RECORD_TIME_MS 2000  // 录音2秒
#define TEST_AUDIO_CHUNK_SIZE 512 // 每次发送512字节

// ==================== 全局变量 ====================
WiFiClientSecure espClient;
PubSubClient mqtt(espClient);

// ==================== 函数声明 ====================
void initI2SInput();
void initI2SOutput();
void playPcmData(uint8_t* data, int len);
void mqtt_reconnect();
void mqtt_callback(char* topic, byte* payload, unsigned int length);
void testRecordAndPlaybackTask(void* pvParameters);

// ==================== 初始化 ====================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n========================================");
    Serial.println("  录音回环测试程序");
    Serial.println("========================================\n");

    // 初始化I2S
    initI2SOutput();
    initI2SInput();
    Serial.println("[I2S] 初始化完成\n");

    // 连接WiFi
    Serial.printf("[WiFi] 连接 %s...\n", WIFI_SSID);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    int retry = 0;
    while (WiFi.status() != WL_CONNECTED && retry < 30) {
        delay(500);
        Serial.print(".");
        retry++;
    }

    if (WiFi.status() == WL_CONNECTED) {
        Serial.printf("\n[WiFi] 已连接，IP: %s\n", WiFi.localIP().toString().c_str());
    } else {
        Serial.println("\n[WiFi] 连接失败，重启...");
        ESP.restart();
    }

    // 同步NTP时间（TLS需要）
    configTime(8 * 3600, 0, "ntp.ntsc.ac.cn", "cn.pool.ntp.org");
    struct tm timeinfo;
    int ntp_retry = 0;
    while (!getLocalTime(&timeinfo) && ntp_retry < 10) {
        delay(500);
        ntp_retry++;
    }
    if (ntp_retry < 10) {
        Serial.printf("[NTP] 时间同步: %04d-%02d-%02d %02d:%02d:%02d\n",
                      timeinfo.tm_year + 1900, timeinfo.tm_mon + 1, timeinfo.tm_mday,
                      timeinfo.tm_hour, timeinfo.tm_min, timeinfo.tm_sec);
    }

    // 配置MQTT
    espClient.setInsecure();
    espClient.setHandshakeTimeout(12);
    mqtt.setServer(MQTT_BROKER, MQTT_PORT);
    mqtt.setCallback(mqtt_callback);
    mqtt_reconnect();

    // 启动测试任务
    Serial.println("\n========================================");
    Serial.println("  测试模式：录音回环");
    Serial.println("  按 BOOT 键开始录音...");
    Serial.println("========================================\n");

    xTaskCreatePinnedToCore(testRecordAndPlaybackTask, "TestRecTask", 8192, NULL, 2, NULL, 1);
}

void loop() {
    if (!mqtt.connected()) {
        mqtt_reconnect();
    }
    mqtt.loop();
    delay(10);
}

// ==================== MQTT 相关 ====================
void mqtt_reconnect() {
    while (!mqtt.connected()) {
        Serial.printf("[MQTT] 连接 %s:%d...\n", MQTT_BROKER, MQTT_PORT);
        espClient.setInsecure();

        if (mqtt.connect(MQTT_CLIENT_ID, MQTT_USER, MQTT_PASSWORD)) {
            Serial.println("[MQTT] 已连接！");
            // 订阅测试相关主题
            mqtt.subscribe("blindstick/test/tts/chunk/+");
        } else {
            Serial.printf("[MQTT] 失败 rc=%d，5秒后重试...\n", mqtt.state());
            delay(5000);
        }
    }
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    // 接收后端返回的TTS音频
    if (strncmp(topic, "blindstick/test/tts/chunk/", 26) == 0) {
        Serial.printf("[测试] 收到TTS音频块: %d字节\n", length);
        playPcmData(payload, length);
    }
}

// ==================== 测试任务 ====================
void testRecordAndPlaybackTask(void* pvParameters) {
    const int bufferSize = SAMPLE_RATE * 2 * (TEST_RECORD_TIME_MS / 1000); // 64KB

    while (true) {
        // 等待按键（GPIO 0 是BOOT键）
        if (digitalRead(0) == LOW) {
            delay(50); // 消抖
            if (digitalRead(0) == LOW) {
                Serial.println("\n[测试] ========== 开始录音 ==========");

                // 分配缓冲区
                uint8_t* audioBuffer = (uint8_t*)ps_malloc(bufferSize);
                if (!audioBuffer) {
                    audioBuffer = (uint8_t*)malloc(bufferSize);
                }
                if (!audioBuffer) {
                    Serial.println("[测试] 内存分配失败!");
                    continue;
                }

                // 录音
                size_t totalRead = 0;
                unsigned long startTime = millis();
                while (millis() - startTime < TEST_RECORD_TIME_MS && totalRead < bufferSize) {
                    size_t bytesRead = 0;
                    i2s_read(I2S_PORT_MIC, audioBuffer + totalRead, bufferSize - totalRead, &bytesRead, 50);
                    totalRead += bytesRead;
                }
                Serial.printf("[测试] 录音完成: %d 字节\n", totalRead);

                // 等待按键释放
                while (digitalRead(0) == LOW) delay(10);

                // 通过MQTT发送音频数据
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
                        delay(10);
                    }
                    Serial.printf("[测试] 音频发送完成，共%d块\n", chunks);

                    // 3. 发送结束标记
                    StaticJsonDocument<256> endDoc;
                    endDoc["type"] = "test_record_end";
                    endDoc["chunks"] = chunks;
                    char endBuf[256];
                    size_t endLen = serializeJson(endDoc, endBuf, sizeof(endBuf));
                    mqtt.publish("blindstick/test/audio", endBuf, endLen);
                    Serial.println("[测试] 等待后端处理...");
                } else {
                    Serial.println("[测试] MQTT未连接!");
                }

                free(audioBuffer);
                Serial.println("[测试] ========== 等待播放 ==========\n");
            }
        }
        vTaskDelay(50 / portTICK_PERIOD_MS);
    }
}

// ==================== I2S 相关 ====================
void initI2SInput() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
        .sample_rate = SAMPLE_RATE,
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

    esp_err_t err = i2s_driver_install(I2S_PORT_MIC, &i2s_config, 0, NULL);
    if (err != ESP_OK) {
        Serial.printf("[I2S-MIC] 安装失败: %d\n", err);
        return;
    }
    err = i2s_set_pin(I2S_PORT_MIC, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[I2S-MIC] 引脚失败: %d\n", err);
        return;
    }
    Serial.println("[I2S-MIC] 麦克风初始化成功");
}

void initI2SOutput() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
        .sample_rate = SAMPLE_RATE,
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
        Serial.printf("[I2S-OUT] 安装失败: %d\n", err);
        return;
    }
    err = i2s_set_pin(I2S_PORT_OUT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[I2S-OUT] 引脚失败: %d\n", err);
        return;
    }
    Serial.println("[I2S-OUT] 扬声器初始化成功");
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
