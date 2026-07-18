#!/usr/bin/env python3
"""
生成本地完整语音数据
将常用告警语音预存到ESP32，实现<100ms响应

使用方法:
1. 确保安装了requests: pip install requests
2. 运行: python generate_local_voices.py
3. 复制生成的C数组到 esp32_upload.ino 中替换占位数据
4. 确保定义了 LOCAL_VOICE_ENABLED 宏
"""

import requests
import urllib3
urllib3.disable_warnings()

# 百度API配置
BAIDU_API_KEY = "Xbxnhkwb2sxtB6HbH5BUTlUG"
BAIDU_SECRET_KEY = "Tw485P2BFGpPu8WeOVP6hy4S1BHqG4ON"

# 要生成的完整语音列表
VOICES = [
    ("前方有障碍物，请向左绕行", "voice_left"),
    ("前方有障碍物，请向右绕行", "voice_right"),
    ("前方有障碍物，请注意避让", "voice_front"),
    ("左前方有障碍物，请向右绕行", "voice_frontleft"),
    ("右前方有障碍物，请向左绕行", "voice_frontright"),
    ("左侧有障碍物，请向右绕行", "voice_leftside"),
    ("右侧有障碍物，请向左绕行", "voice_rightside"),
]

def get_token():
    """获取百度Access Token"""
    url = "https://aip.baidubce.com/oauth/2.0/token"
    params = {
        "grant_type": "client_credentials",
        "client_id": BAIDU_API_KEY,
        "client_secret": BAIDU_SECRET_KEY
    }
    try:
        resp = requests.post(url, params=params, timeout=10, verify=False)
        return resp.json().get("access_token")
    except Exception as e:
        print(f"[错误] 获取Token失败: {e}")
        return None

def generate_voice(text, array_name, token):
    """生成语音并转换为C数组"""
    url = "https://tsn.baidu.com/text2audio"
    payload = {
        "tex": text,
        "tok": token,
        "cuid": "blindstick",
        "ctp": 1,
        "lan": "zh",
        "spd": 5,    # 语速适中
        "pit": 5,    # 音调适中
        "vol": 9,    # 音量较大
        "per": 1,    # 女声
        "aue": 6     # WAV格式
    }

    print(f"\n[生成] '{text}' -> {array_name}")

    try:
        resp = requests.post(url, data=payload, timeout=20, verify=False)

        if 'audio' not in resp.headers.get('Content-Type', ''):
            print(f"[错误] TTS合成失败: {resp.text[:200]}")
            return None

        # 去掉WAV头，获取PCM数据
        data = resp.content[44:]

        # 转换为16位有符号整数
        samples = []
        for i in range(0, len(data), 2):
            if i + 1 < len(data):
                sample = data[i] | (data[i+1] << 8)
                if sample > 32767:
                    sample -= 65536
                samples.append(sample)

        duration = len(samples) / 16000
        print(f"[成功] 采样点: {len(samples)}, 时长: {duration:.2f}秒")

        # 生成C数组代码
        lines = []
        lines.append(f"// {text}")
        lines.append(f"// 时长: {duration:.2f}秒, 采样点: {len(samples)}")
        lines.append(f"const int16_t {array_name}[] = {{")

        # 每行6个数据（便于复制粘贴）
        for i in range(0, len(samples), 6):
            row = samples[i:i+6]
            row_str = ", ".join(f"{s:6d}" for s in row)
            lines.append(f"    {row_str},")

        lines.append(f"}};")
        lines.append(f"const int {array_name}_len = sizeof({array_name}) / sizeof({array_name}[0]);\n")

        return "\n".join(lines)

    except Exception as e:
        print(f"[错误] 请求异常: {e}")
        return None

def main():
    print("="*70)
    print("       本地完整语音生成工具")
    print("="*70)
    print("\n步骤:")
    print("1. 连接网络")
    print("2. 运行此脚本")
    print("3. 复制生成的C数组")
    print("4. 粘贴到 esp32_upload.ino 替换占位数据")
    print("5. 确保定义了 LOCAL_VOICE_ENABLED")
    print("="*70)

    # 获取Token
    print("\n[步骤1] 获取百度Token...")
    token = get_token()
    if not token:
        print("[失败] 无法获取Token，请检查API Key")
        return
    print("[成功] Token获取成功")

    # 生成所有语音
    print(f"\n[步骤2] 生成 {len(VOICES)} 条语音...")

    all_voices = []
    for text, name in VOICES:
        code = generate_voice(text, name, token)
        if code:
            all_voices.append(code)

    if not all_voices:
        print("\n[失败] 没有生成任何语音")
        return

    # 保存到文件
    print("\n[步骤3] 保存到文件...")
    output = "local_voice_data.txt"
    with open(output, "w", encoding="utf-8") as f:
        f.write("// ==================== 本地完整语音数据 ====================\n")
        f.write("// 将此文件内容复制到 esp32_upload.ino 替换占位数据\n")
        f.write("// ========================================================\n\n")
        for voice in all_voices:
            f.write(voice + "\n")

    print(f"[成功] 已保存到: {output}")

    # 打印摘要
    print("\n" + "="*70)
    print("生成摘要:")
    print(f"- 成功生成: {len(all_voices)}/{len(VOICES)} 条语音")
    print(f"- 输出文件: {output}")
    print("\n下一步:")
    print(f"1. 打开 {output}")
    print("2. 复制所有内容")
    print("3. 粘贴到 esp32_upload.ino 中替换占位数据")
    print("4. 上传ESP32代码")
    print("="*70)

if __name__ == "__main__":
    main()
