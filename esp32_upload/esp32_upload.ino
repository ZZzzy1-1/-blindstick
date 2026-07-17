#include "driver/i2s.h"
#include <math.h>

// ===================== 引脚 =====================
#define BCLK        2    // INMP441 SCK
#define LRCK        5    // INMP441 WS
#define MIC_IN      8    // INMP441 SD

#define SPK_BCLK    47   // MAX98357 BCLK
#define SPK_LRCK    41   // MAX98357 LRC
#define SPK_OUT     21   // MAX98357 DIN

#define SAMPLE_RATE 16000

// 降噪参数
#define NOISE_GATE_BASE  350   // 基础静音阈值
#define VOLUME           3     // 降低放大倍数，减少噪声放大
#define SMOOTH_FACTOR    0.25  // 低通平滑系数
#define NOISE_ATTACK     0.08  // 开门速度
#define NOISE_RELEASE    0.03  // 关门缓降，消除咔哒声

// I2S0 麦克风输入
i2s_config_t i2s_in_cfg = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX),
  .sample_rate = SAMPLE_RATE,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
  .communication_format = I2S_COMM_FORMAT_STAND_I2S,
  .intr_alloc_flags = ESP_INTR_FLAG_IRAM,
  .dma_buf_count = 8,    // 加大缓冲区，减少断音杂音
  .dma_buf_len = 512,
  .use_apll = true,      // APLL时钟，音频采样更纯净，减少高频噪声
  .tx_desc_auto_clear = false,
  .fixed_mclk = 0
};
i2s_pin_config_t mic_pin = {
  .bck_io_num = BCLK,
  .ws_io_num = LRCK,
  .data_out_num = -1,
  .data_in_num = MIC_IN
};

// I2S1 喇叭输出
i2s_config_t i2s_out_cfg = {
  .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX),
  .sample_rate = SAMPLE_RATE,
  .bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT,
  .channel_format = I2S_CHANNEL_FMT_ONLY_LEFT,
  .communication_format = I2S_COMM_FORMAT_STAND_I2S,
  .intr_alloc_flags = ESP_INTR_FLAG_IRAM,
  .dma_buf_count = 8,
  .dma_buf_len = 512,
  .use_apll = true,
  .tx_desc_auto_clear = true,
  .fixed_mclk = 0
};
i2s_pin_config_t spk_pin = {
  .bck_io_num = SPK_BCLK,
  .ws_io_num = SPK_LRCK,
  .data_out_num = SPK_OUT,
  .data_in_num = -1
};

int16_t buf[512];
size_t rlen;

// 降噪全局状态变量
int32_t dc_offset = 0;        // 麦克风直流偏移
int32_t smooth_prev = 0;      // 低通滤波缓存
float noise_gate_env = 0.0f;   // 噪声门包络
float env_smooth = 0.0f;       // 音量包络平滑

void setup() {
  Serial.begin(115200);
  i2s_driver_install(I2S_NUM_0,&i2s_in_cfg,0,NULL);
  i2s_set_pin(I2S_NUM_0,&mic_pin);
  i2s_driver_install(I2S_NUM_1,&i2s_out_cfg,0,NULL);
  i2s_set_pin(I2S_NUM_1,&spk_pin);
  Serial.println("✅ 深度降噪回环已加载");
}

void loop() {
  i2s_read(I2S_NUM_0, buf, sizeof(buf), &rlen, portMAX_DELAY);
  int samples = rlen / 2;

  for (int i = 0; i < samples; i++) {
    int32_t sample = buf[i];

    // 1. 消除INMP441直流偏移（解决持续底噪核心）
    dc_offset = dc_offset * 0.999f + sample * 0.001f;
    sample -= dc_offset;

    // 2. 一阶低通滤波，滤高频沙沙噪声
    smooth_prev = smooth_prev * (1 - SMOOTH_FACTOR) + sample * SMOOTH_FACTOR;
    sample = smooth_prev;

    // 3. 计算音量包络，做平滑噪声门（无咔哒爆音）
    int32_t abs_sig = abs(sample);
    if (abs_sig > env_smooth) {
      env_smooth = env_smooth * (1 - NOISE_ATTACK) + abs_sig * NOISE_ATTACK;
    } else {
      env_smooth = env_smooth * (1 - NOISE_RELEASE) + abs_sig * NOISE_RELEASE;
    }

    // 4. 软噪声门：低于阈值缓慢衰减至0，不硬截断
    float gate_ratio = 1.0f;
    if (env_smooth < NOISE_GATE_BASE) {
      gate_ratio = env_smooth / (float)NOISE_GATE_BASE;
    }
    sample = sample * gate_ratio;

    // 5. 音量放大
    sample = sample * VOLUME;

    // 6. 限幅防破音
    if(sample > 32767) sample = 32767;
    if(sample < -32768) sample = -32768;

    buf[i] = sample;
  }

  i2s_write(I2S_NUM_1, buf, rlen, &rlen, portMAX_DELAY);
}