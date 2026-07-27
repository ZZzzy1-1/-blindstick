# 导盲杖音频生成指南

## 文件说明

| 文件 | 用途 |
|------|------|
| `generate_audio.py` | 调用百度TTS API生成真实语音 |
| `convert_audio.py` | 转换已有音频文件为PCM格式 |
| `generate_test_audio.py` | 生成测试用的蜂鸣声（无需网络） |

## 方案一：使用百度TTS生成语音（推荐）

### 1. 安装依赖
```bash
pip install requests
```

### 2. 运行生成脚本
```bash
cd server
python generate_audio.py
```

### 3. 输出文件
- 生成在 `./k230_audio/` 目录
- 14个 `.pcm` 文件
- 格式：16kHz 16bit 单声道 PCM

## 方案二：转换已有音频文件

如果你有现成的MP3/WAV文件：

```bash
# 将音频文件放入一个目录，例如 ./my_audio/
python convert_audio.py ./my_audio/
```

文件名包含关键词即可自动匹配类别：
- `盲道.mp3` → `blind_track.pcm`
- `红灯.wav` → `red_light.pcm`
- ...

## 方案三：快速测试音频（无需网络）

生成不同频率的蜂鸣声用于测试传输功能：

```bash
python generate_test_audio.py
```

## 方案四：使用ffmpeg手动转换

### 单个文件转换
```bash
ffmpeg -i input.mp3 -ar 16000 -ac 1 -f s16le output.pcm
```

### 批量转换
```bash
# Windows PowerShell
Get-ChildItem *.mp3 | ForEach-Object {
    ffmpeg -i $_.Name -ar 16000 -ac 1 -f s16le ($_.BaseName + ".pcm")
}

# Linux/Mac Bash
for f in *.mp3; do
    ffmpeg -i "$f" -ar 16000 -ac 1 -f s16le "${f%.mp3}.pcm"
done
```

## 上传到K230 SD卡

### 方法1：直接复制
1. 将SD卡从K230取出，插入电脑
2. 复制PCM文件到SD卡：
```bash
mkdir /path/to/sdcard/audio
cp ./k230_audio/*.pcm /path/to/sdcard/audio/
```

### 方法2：通过K230串口上传（使用mpremote）
```bash
# 安装mpremote
pip install mpremote

# 连接到K230
mpremote connect COM3

# 创建目录
mpremote fs mkdir /sdcard/audio

# 上传文件
mpremote fs cp blind_track.pcm /sdcard/audio/
mpremote fs cp curb.pcm /sdcard/audio/
# ... 上传其他文件

# 或者批量上传
mpremote fs cp ./k230_audio/*.pcm /sdcard/audio/
```

### 方法3：通过CanMV IDE
1. 打开CanMV IDE
2. 连接K230
3. 使用文件管理器创建 `/sdcard/audio/` 目录
4. 拖拽PCM文件到该目录

## 验证音频文件

### 在电脑上播放测试
```bash
# 使用ffplay播放PCM文件
ffplay -f s16le -ar 16000 -ac 1 blind_track.pcm
```

### 检查文件格式
```bash
# 查看文件大小（正常1-2秒语音约32-64KB）
ls -la *.pcm

# 文件大小计算公式：
# 大小(字节) = 采样率(16000) × 时长(秒) × 声道数(1) × 位深(2字节)
# 1秒 = 16000 × 1 × 2 = 32000 字节
```

## 14类目标音频文件列表

| 文件名 | 中文内容 | 建议时长 |
|--------|----------|----------|
| blind_track.pcm | "前方有盲道" | 1-2秒 |
| curb.pcm | "注意马路牙子" | 1-2秒 |
| crosswalk.pcm | "前方有斑马线" | 1-2秒 |
| pole.pcm | "注意立柱" | 1-2秒 |
| ashcan.pcm | "前方有垃圾桶" | 1-2秒 |
| reflective_cone.pcm | "注意反光锥" | 1-2秒 |
| red_light.pcm | "红灯" | 1秒 |
| yellow_light.pcm | "黄灯" | 1秒 |
| green_light.pcm | "绿灯" | 1秒 |
| stop_sign.pcm | "注意标志牌" | 1-2秒 |
| person.pcm | "前方有行人" | 1-2秒 |
| vehicle.pcm | "注意车辆" | 1-2秒 |
| stairs.pcm | "注意楼梯台阶" | 1-2秒 |
| puddle.pcm | "前方有水坑" | 1-2秒 |

## 故障排除

### 问题1：generate_audio.py提示缺少ffmpeg
**解决：** 安装ffmpeg
- Windows: `choco install ffmpeg` 或从官网下载
- Mac: `brew install ffmpeg`
- Ubuntu: `sudo apt install ffmpeg`

### 问题2：百度TTS返回错误
**检查：**
1. API密钥是否正确
2. 网络连接是否正常
3. 免费额度是否用完

### 问题3：ESP32播放无声
**检查：**
1. PCM文件是否正确放入SD卡 `/sdcard/audio/` 目录
2. 文件名是否完全匹配（区分大小写）
3. 串口连接是否正常
4. 使用测试音频排除硬件问题

### 问题4：音频播放有杂音
**解决：**
- 确保PCM格式正确：16kHz 16bit 单声道
- 检查串口波特率是否一致（115200）
- 增大帧发送间隔（修改 `AUDIO_SEND_DELAY_MS`）
