// ==================== 流式TTS播放管理器（简化版）====================
// 功能：
// 1. 接收流式音频数据，边接收边播放
// 2. 高优先级消息立即打断当前播放
// 3. 移除队列逻辑，简化代码

// ==================== 配置 ====================
#define STREAM_BUFFER_SIZE    8192   // 流式播放缓冲区（8KB）
#define STREAM_CHUNK_SIZE     1024   // 每次接收的音频块大小
#define STREAM_PLAY_THRESHOLD 2048   // 开始播放的阈值（缓冲2KB后开始播放）

// ==================== 全局状态 ====================
volatile bool stream_playing = false;           // 是否正在播放
volatile int  stream_priority = 0;              // 当前播放优先级 0=低 1=中 2=高
volatile int  stream_buffer_used = 0;           // 缓冲区已使用大小
volatile unsigned long stream_session_id = 0;   // 当前会话ID

// 流式播放缓冲区（使用PSRAM或普通内存）
uint8_t* stream_buffer = NULL;

// 音频格式
#define AUDIO_FORMAT_PCM_16K  0  // PCM 16kHz 16bit
#define AUDIO_FORMAT_WAV      1  // WAV格式
volatile int audio_format = AUDIO_FORMAT_PCM_16K;

// ==================== 优先级定义 ====================
#define PRIO_LOW     0   // 导航
#define PRIO_NORMAL  1   // 对话
#define PRIO_HIGH    2   // 雷达告警

const char* getPrioName(int p) {
    switch(p) {
        case PRIO_HIGH: return "高(雷达)";
        case PRIO_NORMAL: return "中(对话)";
        case PRIO_LOW: return "低(导航)";
        default: return "未知";
    }
}

// ==================== I2S播放函数 ====================
/**
 * 播放PCM数据
 * @param data PCM数据指针
 * @param len  数据长度（字节）
 */
void playPcmData(uint8_t* data, int len) {
    if (!data || len < 2) return;

    int16_t* samples = (int16_t*)data;
    int num_samples = len / 2;

    // 应用音量增益并播放
    for (int i = 0; i < num_samples; i += 512) {
        int16_t temp_buffer[512];
        int chunk_samples = min(512, num_samples - i);

        for (int j = 0; j < chunk_samples; j++) {
            int32_t sample = (int32_t)(samples[i + j] * VOLUME_GAIN);
            if (sample > 32767) sample = 32767;
            if (sample < -32768) sample = -32768;
            temp_buffer[j] = (int16_t)sample;
        }

        size_t bytes_written = 0;
        i2s_write(I2S_PORT_OUT, temp_buffer, chunk_samples * 2, &bytes_written, portMAX_DELAY);
    }
}

/**
 * 清空I2S缓冲区，停止当前播放
 */
void stopCurrentPlayback() {
    i2s_zero_dma_buffer(I2S_PORT_OUT);
    stream_playing = false;
    stream_buffer_used = 0;
    Serial.println("[流式TTS] 停止当前播放");
}

// ==================== 流式TTS初始化 ====================
void initStreamingTTS() {
    // 分配流式缓冲区
    if (ESP.getPsramSize() > 0) {
        stream_buffer = (uint8_t*)ps_malloc(STREAM_BUFFER_SIZE);
    } else {
        stream_buffer = (uint8_t*)malloc(STREAM_BUFFER_SIZE);
    }

    if (stream_buffer) {
        Serial.printf("[流式TTS] 缓冲区分配成功: %d字节\n", STREAM_BUFFER_SIZE);
    } else {
        Serial.println("[流式TTS] 缓冲区分配失败！");
    }

    stream_playing = false;
    stream_priority = 0;
    stream_buffer_used = 0;
}

// ==================== 流式播放控制消息处理 ====================
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
        // 新的流式播放开始
        Serial.printf("[流式TTS] 开始新会话 priority=%s session=%lu\n",
                     getPrioName(new_priority), session_id);

        // 检查是否需要打断
        if (stream_playing) {
            if (new_priority >= stream_priority) {
                // 同等或更高优先级，打断当前
                Serial.printf("[流式TTS] 打断当前%s播放\n", getPrioName(stream_priority));
                stopCurrentPlayback();
            } else {
                // 低优先级，忽略
                Serial.printf("[流式TTS] 忽略低优先级%s（当前%s）\n",
                             getPrioName(new_priority), getPrioName(stream_priority));
                return;
            }
        }

        // 开始新会话
        stream_playing = true;
        stream_priority = new_priority;
        stream_session_id = session_id;
        stream_buffer_used = 0;

        // 暂停语音识别
        if (VoiceTaskHandle != NULL) {
            vTaskSuspend(VoiceTaskHandle);
            webSocket.disconnect();
            Serial.println("[流式TTS] 语音识别已暂停");
        }

    } else if (strcmp(type, "stream_end") == 0) {
        // 流式播放结束
        int segments = doc["segments"] | 0;
        Serial.printf("[流式TTS] 会话结束，共%d段\n", segments);

        // 播放缓冲区剩余数据
        if (stream_buffer_used > 0 && stream_playing) {
            Serial.printf("[流式TTS] 播放剩余%d字节\n", stream_buffer_used);
            playPcmData(stream_buffer, stream_buffer_used);
        }

        // 恢复语音识别
        if (VoiceTaskHandle != NULL && stream_priority < PRIO_HIGH) {
            vTaskResume(VoiceTaskHandle);
            Serial.println("[流式TTS] 语音识别已恢复");
        }

        stream_playing = false;
        stream_buffer_used = 0;

    } else if (strcmp(type, "interrupt") == 0) {
        // 打断信号
        Serial.printf("[流式TTS] 收到打断信号，新优先级=%s\n", getPrioName(new_priority));
        stopCurrentPlayback();

        // 恢复语音识别
        if (VoiceTaskHandle != NULL) {
            vTaskResume(VoiceTaskHandle);
        }
    }
}

// ==================== 流式音频数据处理 ====================
void handleStreamAudio(const char* topic, byte* payload, unsigned int length) {
    if (!stream_playing) {
        // 没有活动的流式会话，忽略
        return;
    }

    // 检查会话是否匹配（通过topic中的segment索引）
    // topic格式: blindstick/tts/stream/{segment_idx}
    int segment_idx = 0;
    const char* last_slash = strrchr(topic, '/');
    if (last_slash) {
        segment_idx = atoi(last_slash + 1);
    }

    Serial.printf("[流式TTS] 收到第%d段: %d字节\n", segment_idx, length);

    // 直接播放（边收边播模式）
    // 对于高优先级（雷达告警），立即播放，不缓冲
    if (stream_priority == PRIO_HIGH) {
        playPcmData(payload, length);
        return;
    }

    // 对于普通优先级，使用缓冲播放以获得更流畅的效果
    // 将数据加入缓冲区
    if (stream_buffer_used + length <= STREAM_BUFFER_SIZE) {
        memcpy(stream_buffer + stream_buffer_used, payload, length);
        stream_buffer_used += length;

        // 如果缓冲区超过阈值，播放一半数据
        if (stream_buffer_used >= STREAM_PLAY_THRESHOLD) {
            int play_size = stream_buffer_used / 2;
            playPcmData(stream_buffer, play_size);

            // 移动剩余数据到缓冲区头部
            memmove(stream_buffer, stream_buffer + play_size, stream_buffer_used - play_size);
            stream_buffer_used -= play_size;
        }
    } else {
        // 缓冲区满了，先播放一半
        int play_size = STREAM_BUFFER_SIZE / 2;
        playPcmData(stream_buffer, play_size);

        // 移动剩余数据
        memmove(stream_buffer, stream_buffer + play_size, stream_buffer_used - play_size);
        stream_buffer_used -= play_size;

        // 添加新数据
        if (stream_buffer_used + length <= STREAM_BUFFER_SIZE) {
            memcpy(stream_buffer + stream_buffer_used, payload, length);
            stream_buffer_used += length;
        }
    }
}

// ==================== 雷达告警语音播报 ====================
/**
 * 播报障碍物告警（高优先级，立即打断）
 */
void announceObstacleStreaming(float distance, const char* direction) {
    char text[64];

    // 根据距离选择提示语
    if (distance < FRONT_CRITICAL_CM) {
        snprintf(text, sizeof(text), "注意！前方%.0f厘米有障碍物，请立即避让！", distance);
    } else if (distance < ALERT_DIST_CM) {
        snprintf(text, sizeof(text), "前方%.0f厘米有%s障碍物", distance, direction);
    } else {
        return;  // 超出告警范围
    }

    Serial.printf("[雷达告警] %s\n", text);

    // 通过MQTT发送高优先级TTS请求到代理服务器
    // 代理服务器会流式合成并通过MQTT发送音频
    StaticJsonDocument<256> doc;
    doc["text"] = text;
    doc["priority"] = PRIO_HIGH;

    char jsonBuffer[256];
    serializeJson(doc, jsonBuffer);

    // 发布到请求主题，代理服务器会处理
    mqtt.publish("blindstick/tts/request", jsonBuffer);
}

// ==================== 导航语音播报 ====================
/**
 * 播报导航信息（普通优先级）
 */
void announceNavigation(const char* text) {
    Serial.printf("[导航] %s\n", text);

    StaticJsonDocument<256> doc;
    doc["text"] = text;
    doc["priority"] = PRIO_NORMAL;

    char jsonBuffer[256];
    serializeJson(doc, jsonBuffer);

    mqtt.publish("blindstick/tts/request", jsonBuffer);
}

// ==================== MQTT订阅更新 ====================
void subscribeMQTTTopics() {
    // 原有订阅...
    mqtt.subscribe(MQTT_TOPIC_SENSORS);
    mqtt.subscribe(MQTT_TOPIC_NAV_STEPS);
    mqtt.subscribe(MQTT_TOPIC_TTS_REQ);

    // 新增流式TTS主题
    mqtt.subscribe("blindstick/tts/control");       // 控制消息（开始/结束/打断）
    mqtt.subscribe("blindstick/tts/stream/+");      // 音频数据流（使用通配符）

    Serial.println("[MQTT] 已订阅流式TTS主题");
}

// ==================== MQTT消息处理更新 ====================
void mqttCallbackStreaming(char* topic, byte* payload, unsigned int length) {
    // 处理流式TTS控制消息
    if (strcmp(topic, "blindstick/tts/control") == 0) {
        handleStreamControl((const char*)payload, length);
        return;
    }

    // 处理流式TTS音频数据
    if (strncmp(topic, "blindstick/tts/stream/", 22) == 0) {
        handleStreamAudio(topic, payload, length);
        return;
    }

    // 原有消息处理...
    // ... 其他topic的处理代码保持不变 ...
}

// ==================== setup()中的初始化 ====================
void setupStreamingTTS() {
    initStreamingTTS();
}
