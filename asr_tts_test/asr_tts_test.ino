/**
 * 语音识别 + 百度TTS合成播报测试程序
 *
 * 功能：
 * 1. 录音2秒（I2S麦克风INMP441）
 * 2. 调用百度ASR API进行语音识别
 * 3. 将识别到的文字用百度TTS合成语音
 * 4. 通过I2S播放音频（MAX98357功放）
 *
 * 硬件接线：
 * === INMP441 麦克风 ===
 * - VDD → 3.3V
 * - WS  → GPIO5 (LRCK)
 * - SCK → GPIO2 (BCLK)
 * - SD  → GPIO8 (MIC_IN)
 * - GND → GND
 * - L/R → GND (左声道)
 *
 * === MAX98357 功放 ===
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
#include <HTTPClient.h>
#include <driver/i2s.h>
#include <ArduinoJson.h>

// ==================== 配置 ====================
const char* WIFI_SSID     = "ZZY";
const char* WIFI_PASSWORD = "zzy060630";

// 百度API密钥（语音识别和TTS共用）
const String BAIDU_API_KEY    = "Xbxnhkwb2sxtB6HbH5BUTlUG";
const String BAIDU_SECRET_KEY = "Tw485P2BFGpPu8WeOVP6hy4S1BHqG4ON";

// I2S麦克风引脚 (INMP441)
#define I2S_WS_PIN      5   // LRCK
#define I2S_SCK_PIN     2   // BCLK
#define I2S_SD_PIN      8   // MIC_IN
#define I2S_PORT_MIC    I2S_NUM_0

// I2S扬声器引脚 (MAX98357)
#define I2S_BCK_PIN     47  // SPK_BCLK
#define I2S_WS_OUT_PIN  41  // SPK_LRCK
#define I2S_DATA_PIN    21  // SPK_OUT
#define I2S_PORT_OUT    I2S_NUM_1

#define VOLUME_GAIN     0.6  // 音量增益（0.0-1.0）

// 录音配置 - 减小到2秒以节省内存
#define RECORD_TIME_MS  2000  // 录音时长2秒（原来3秒）
#define SAMPLE_RATE     16000
#define BITS_PER_SAMPLE 16

// ==================== 全局变量 ====================
String baidu_access_token = "";
unsigned long token_expire_time = 0;

// ==================== 函数声明 ====================
bool connectWiFi();
String getBaiduToken();
String recognizeSpeech();  // 语音识别
bool synthesizeAndPlay(const char* text);  // TTS合成并播放
String base64Encode(const uint8_t* data, size_t len);
void initI2SInput();   // 初始化麦克风
void initI2SOutput();  // 初始化扬声器
void playPcmData(uint8_t* data, int len, int sampleRate);

// ==================== 初始化 ====================
void setup() {
    Serial.begin(115200);
    delay(1000);

    Serial.println("\n========================================");
    Serial.println("  语音识别 + 百度TTS 测试程序");
    Serial.println("========================================\n");

    // 初始化I2S（麦克风和扬声器）
    initI2SOutput();
    initI2SInput();
    Serial.println("[I2S] 初始化完成\n");

    // 播放启动提示音
    playStartupTone();

    // 连接WiFi
    if (!connectWiFi()) {
        Serial.println("[错误] WiFi连接失败，停止运行");
        return;
    }

    Serial.println("\n========================================");
    Serial.println("  准备开始语音识别测试");
    Serial.println("  请对着麦克风说话...");
    Serial.println("========================================\n");
}

void loop() {
    // 每次循环：录音 -> 识别 -> 播报
    Serial.println("\n>>> 请说话（3秒录音）...");

    // 1. 语音识别
    String recognizedText = recognizeSpeech();

    if (recognizedText.length() > 0) {
        Serial.printf("\n[识别结果] %s\n", recognizedText.c_str());

        // 2. TTS合成并播放
        String ttsText = "您说的是：" + recognizedText;
        Serial.println("[TTS] 正在合成语音...");

        if (synthesizeAndPlay(ttsText.c_str())) {
            Serial.println("[完成] 播放完成\n");
        } else {
            Serial.println("[错误] TTS合成失败\n");
        }
    } else {
        Serial.println("\n[提示] 未能识别到语音，请重试\n");
    }

    // 等待3秒后继续下一次识别
    delay(3000);
}

// ==================== 语音识别（百度ASR REST API）====================

/**
 * 录制音频并进行语音识别
 */
String recognizeSpeech() {
    // 分配录音缓冲区 (2秒 16kHz 16bit = 64KB)
    const int bufferSize = SAMPLE_RATE * 2 * (RECORD_TIME_MS / 1000);
    uint8_t* audioBuffer = NULL;

    // 优先使用PSRAM（外部RAM，空间更大）
    if (ESP.getPsramSize() > 0) {
        audioBuffer = (uint8_t*)ps_malloc(bufferSize);
        if (audioBuffer) {
            Serial.println("[ASR] 使用PSRAM分配录音缓冲区");
        }
    }

    // PSRAM不可用或分配失败，使用普通内存
    if (!audioBuffer) {
        audioBuffer = (uint8_t*)malloc(bufferSize);
        if (audioBuffer) {
            Serial.printf("[ASR] 使用普通内存分配录音缓冲区: %d字节\n", bufferSize);
        }
    }

    if (!audioBuffer) {
        Serial.println("[ASR] 内存分配失败");
        return "";
    }

    // 录音
    Serial.println("[ASR] 开始录音...");
    size_t totalRead = 0;
    unsigned long startTime = millis();

    while (millis() - startTime < RECORD_TIME_MS && totalRead < bufferSize) {
        size_t bytesRead = 0;
        i2s_read(I2S_PORT_MIC, audioBuffer + totalRead, bufferSize - totalRead, &bytesRead, 50);
        totalRead += bytesRead;
    }

    Serial.printf("[ASR] 录音完成: %d 字节\n", totalRead);

    // 检查录音数据是否有效
    int nonZeroCount = 0;
    int16_t* samples = (int16_t*)audioBuffer;
    for (int i = 0; i < totalRead / 2; i++) {
        if (samples[i] != 0 && samples[i] != -1) nonZeroCount++;
    }
    Serial.printf("[ASR] 非零样本数: %d\n", nonZeroCount);

    if (nonZeroCount < 100) {
        Serial.println("[ASR] 警告: 录音数据可能为空，请检查麦克风");
    }

    // 获取Token
    String token = getBaiduToken();
    if (token.length() == 0) {
        Serial.println("[ASR] 无法获取Token");
        free(audioBuffer);
        return "";
    }

    // Base64编码
    Serial.printf("[ASR] 开始Base64编码，输入: %d 字节...\n", totalRead);
    Serial.printf("[ASR] 可用内存: %d 字节，PSRAM: %d 字节\n", ESP.getFreeHeap(), ESP.getFreePsram());

    String base64Audio = base64Encode(audioBuffer, totalRead);

    Serial.printf("[ASR] Base64编码完成: %d 字符 (预期: %d)\n",
                  base64Audio.length(), ((totalRead + 2) / 3) * 4);

    if (base64Audio.length() == 0) {
        Serial.println("[ASR] Base64编码失败!");
        free(audioBuffer);
        return "";
    }

    free(audioBuffer);

    // 发送ASR请求
    Serial.println("[ASR] 发送识别请求...");
    Serial.printf("[ASR] 参数: format=pcm, rate=%d, channel=1, len=%d\n", SAMPLE_RATE, totalRead);

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;
    if (!http.begin(client, "https://vop.baidu.com/server_api")) {
        Serial.println("[ASR] HTTP初始化失败");
        return "";
    }

    http.addHeader("Content-Type", "application/json");
    http.setTimeout(15000);

    // 构建JSON请求 - 手动拼接避免ArduinoJson容量限制
    // 格式: {"format":"pcm","rate":16000,"channel":1,"cuid":"esp32_test","token":"xxx","dev_pid":1537,"speech":"BASE64...","len":64000}

    String jsonPayload;
    jsonPayload.reserve(90000);  // 预分配约90KB内存
    jsonPayload = "{\"format\":\"pcm\",\"rate\":16000,\"channel\":1,\"cuid\":\"esp32_test\",\"token\":\"";
    jsonPayload += token;
    jsonPayload += "\",\"dev_pid\":1537,\"speech\":\"";
    jsonPayload += base64Audio;
    jsonPayload += "\",\"len\":";
    jsonPayload += totalRead;
    jsonPayload += "}";

    Serial.printf("[ASR] JSON大小: %d 字节\n", jsonPayload.length());

    if (jsonPayload.length() < 1000) {
        Serial.println("[ASR] 错误: JSON构建失败!");
        return "";
    }

    int httpCode = http.POST(jsonPayload);
    String result = "";

    if (httpCode == 200) {
        String response = http.getString();
        Serial.printf("[ASR] 响应: %s\n", response.c_str());

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
            int errNo = respDoc["err_no"] | -1;
            Serial.printf("[ASR] 识别错误码: %d\n", errNo);
        }
    } else {
        Serial.printf("[ASR] HTTP错误: %d\n", httpCode);
    }

    http.end();
    return result;
}

/**
 * Base64编码 - 使用预分配内存避免动态扩容
 */
String base64Encode(const uint8_t* data, size_t len) {
    static const char base64Chars[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    // 预分配内存: 每3字节编码为4字符，向上取整
    size_t outLen = ((len + 2) / 3) * 4;

    // 使用动态分配的缓冲区（栈上可能溢出）
    char* buffer = (char*)malloc(outLen + 1);
    if (!buffer) {
        Serial.println("[Base64] 内存分配失败");
        return "";
    }

    size_t j = 0;
    for (size_t i = 0; i < len; i += 3) {
        uint8_t b1 = data[i];
        uint8_t b2 = (i + 1 < len) ? data[i + 1] : 0;
        uint8_t b3 = (i + 2 < len) ? data[i + 2] : 0;

        buffer[j++] = base64Chars[(b1 >> 2) & 0x3F];
        buffer[j++] = base64Chars[((b1 << 4) | (b2 >> 4)) & 0x3F];
        buffer[j++] = (i + 1 < len) ? base64Chars[((b2 << 2) | (b3 >> 6)) & 0x3F] : '=';
        buffer[j++] = (i + 2 < len) ? base64Chars[b3 & 0x3F] : '=';
    }
    buffer[j] = '\0';

    String result(buffer);
    free(buffer);
    return result;
}

// ==================== TTS合成与播放 ====================

/**
 * TTS合成并播放
 */
bool synthesizeAndPlay(const char* text) {
    String token = getBaiduToken();
    if (token.length() == 0) {
        Serial.println("[TTS] 无法获取Token");
        return false;
    }

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(15000);

    HTTPClient http;
    String url = "https://tsn.baidu.com/text2audio";

    if (!http.begin(client, url)) {
        Serial.println("[TTS] HTTP初始化失败");
        return false;
    }

    http.setTimeout(15000);

    // 构造POST参数
    String postData = "tex=" + urlEncode(text);
    postData += "&tok=" + token;
    postData += "&cuid=esp32_test";
    postData += "&ctp=1";
    postData += "&lan=zh";
    postData += "&spd=5";  // 语速
    postData += "&pit=5";  // 音调
    postData += "&vol=9";  // 音量
    postData += "&per=0";  // 发音人(0=女声, 1=男声, 3=度逍遥, 4=度丫丫)
    postData += "&aue=6";  // WAV格式

    http.addHeader("Content-Type", "application/x-www-form-urlencoded");

    int httpCode = http.POST(postData);
    if (httpCode != 200) {
        Serial.printf("[TTS] HTTP错误: %d\n", httpCode);
        http.end();
        return false;
    }

    // 检查返回类型
    String contentType = http.header("Content-Type");
    Serial.printf("[TTS] Content-Type: %s\n", contentType.c_str());

    // 读取响应数据
    int len = http.getSize();
    Serial.printf("[TTS] 响应大小: %d 字节\n", len);

    // 判断是否为音频数据
    bool isAudio = (contentType.indexOf("audio") >= 0) ||
                   (len > 1000 && len < 200000);  // 音频通常在1KB-200KB之间

    if (!isAudio) {
        // 不是音频，读取错误信息
        String error = http.getString();
        Serial.printf("[TTS] 合成失败: %s\n", error.c_str());
        http.end();
        return false;
    }

    // 是音频数据
    if (len <= 0 || len > 200000) {
        Serial.printf("[TTS] 音频大小无效: %d\n", len);
        http.end();
        return false;
    }

    Serial.printf("[TTS] 音频大小: %d 字节\n", len);

    // 分配内存
    uint8_t* audioBuffer = (uint8_t*)malloc(len);
    if (!audioBuffer) {
        Serial.println("[TTS] 内存分配失败");
        http.end();
        return false;
    }

    // 读取数据
    WiFiClient* stream = http.getStreamPtr();
    int totalRead = 0;
    while (totalRead < len) {
        int available = stream->available();
        if (available > 0) {
            int toRead = min(available, len - totalRead);
            int r = stream->readBytes(audioBuffer + totalRead, toRead);
            if (r > 0) totalRead += r;
        }
        if (totalRead >= len) break;
        delay(1);
    }

    http.end();

    if (totalRead != len) {
        Serial.printf("[TTS] 读取不完整: %d/%d\n", totalRead, len);
        free(audioBuffer);
        return false;
    }

    // 播放音频
    Serial.println("[TTS] 开始播放...");
    playPcmData(audioBuffer, len, SAMPLE_RATE);

    free(audioBuffer);
    return true;
}

// ==================== I2S和工具函数 ====================

/**
 * 初始化麦克风I2S（输入）
 */
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
        Serial.printf("[I2S-MIC] 驱动安装失败: %d\n", err);
        return;
    }

    err = i2s_set_pin(I2S_PORT_MIC, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[I2S-MIC] 引脚设置失败: %d\n", err);
        return;
    }

    Serial.println("[I2S-MIC] 麦克风初始化成功");
}

/**
 * 初始化扬声器I2S（输出）
 */
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
        Serial.printf("[I2S-OUT] 驱动安装失败: %d\n", err);
        return;
    }

    err = i2s_set_pin(I2S_PORT_OUT, &pin_config);
    if (err != ESP_OK) {
        Serial.printf("[I2S-OUT] 引脚设置失败: %d\n", err);
        return;
    }

    Serial.println("[I2S-OUT] 扬声器初始化成功");
}

/**
 * 播放PCM数据
 */
void playPcmData(uint8_t* audioData, int length, int sampleRate) {
    if (!audioData || length < 100) return;

    // 跳过WAV头（如果有）
    int offset = 0;
    if (length > 44 &&
        audioData[0] == 'R' && audioData[1] == 'I' &&
        audioData[2] == 'F' && audioData[3] == 'F') {
        offset = 44;
    }

    int pcmLength = length - offset;
    int16_t* samples = (int16_t*)(audioData + offset);
    int numSamples = pcmLength / 2;

    // 分块播放
    const int CHUNK_SIZE = 256;
    int16_t tempBuffer[CHUNK_SIZE];

    for (int i = 0; i < numSamples; i += CHUNK_SIZE) {
        int chunkSamples = min(CHUNK_SIZE, numSamples - i);

        // 应用音量增益
        for (int j = 0; j < chunkSamples; j++) {
            int32_t sample = (int32_t)(samples[i + j] * VOLUME_GAIN);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            tempBuffer[j] = (int16_t)sample;
        }

        size_t bytesWritten = 0;
        i2s_write(I2S_PORT_OUT, tempBuffer, chunkSamples * 2, &bytesWritten, portMAX_DELAY);
    }

    // 等待播放完成
    int durationMs = (pcmLength / 32000.0) * 1000;
    delay(durationMs + 200);

    // 清空缓冲区
    i2s_zero_dma_buffer(I2S_PORT_OUT);
}

/**
 * 播放启动提示音
 */
void playStartupTone() {
    Serial.println("[提示] 播放启动音...");
    const int numSamples = SAMPLE_RATE / 4; // 0.25秒
    int16_t* toneBuffer = (int16_t*)malloc(numSamples * 2);

    if (!toneBuffer) return;

    for (int i = 0; i < numSamples; i++) {
        float t = (float)i / SAMPLE_RATE;
        float sample = sin(2 * PI * 800 * t) * 8000.0f;
        toneBuffer[i] = (int16_t)sample;
    }

    size_t written = 0;
    i2s_zero_dma_buffer(I2S_PORT_OUT);
    i2s_write(I2S_PORT_OUT, toneBuffer, numSamples * 2, &written, portMAX_DELAY);

    free(toneBuffer);
    delay(300);
    i2s_zero_dma_buffer(I2S_PORT_OUT);
}

/**
 * 连接WiFi
 */
bool connectWiFi() {
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
        return true;
    } else {
        Serial.println("\n[WiFi] 连接失败");
        return false;
    }
}

/**
 * 获取百度Access Token（带缓存）
 */
String getBaiduToken() {
    // 检查缓存的token是否有效（提前5分钟过期）
    if (baidu_access_token.length() > 0 && millis() < token_expire_time - 300000) {
        return baidu_access_token;
    }

    Serial.println("[Token] 请求新Token...");

    WiFiClientSecure client;
    client.setInsecure();
    client.setTimeout(10000);

    HTTPClient http;
    String url = "https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials";
    url += "&client_id=" + BAIDU_API_KEY;
    url += "&client_secret=" + BAIDU_SECRET_KEY;

    if (!http.begin(client, url)) {
        Serial.println("[Token] HTTP初始化失败");
        return "";
    }

    http.setTimeout(10000);
    int httpCode = http.GET();

    if (httpCode == 200) {
        String response = http.getString();
        StaticJsonDocument<512> doc;
        DeserializationError error = deserializeJson(doc, response);

        if (!error && doc.containsKey("access_token")) {
            baidu_access_token = doc["access_token"].as<String>();
            int expiresIn = doc["expires_in"] | 2592000;
            token_expire_time = millis() + (expiresIn * 1000);
            Serial.println("[Token] 获取成功");
            http.end();
            return baidu_access_token;
        }
    } else {
        Serial.printf("[Token] 错误: %d\n", httpCode);
    }

    http.end();
    return "";
}

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
            sprintf(buf, "%%%02X", (unsigned char)c);
            encoded += buf;
        }
    }
    return encoded;
}
