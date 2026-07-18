#!/usr/bin/env python3
"""
紧急短语音生成工具
生成5个最常用的短语音PCM数据，直接嵌入代码
"""

import requests
import os

BAIDU_API_KEY = "Xbxnhkwb2sxtB6HbH5BUTlUG"
BAIDU_SECRET_KEY = "Tw485P2BFGpPu8WeOVP6hy4S1BHqG4ON"

ALERTS = [
    ("左转", "alert_left"),
    ("右转", "alert_right"),
    ("停", "alert_stop"),
    ("注意", "alert_caution"),
    ("危险", "alert_danger")
]

def get_token():
    url = "https://aip.baidubce.com/oauth/2.0/token"
    params = {
        "grant_type": "client_credentials",
        "client_id": BAIDU_API_KEY,
        "client_secret": BAIDU_SECRET_KEY
    }
    try:
        resp = requests.post(url, params=params, timeout=10, verify=False)
        return resp.json().get("access_token")
    except:
        return None

def generate_short_tts(text, array_name):
    """生成短语音并转换为C数组"""
    token = get_token()
    if not token:
        print(f"[错误] 无法获取Token")
        return None

    url = "https://tsn.baidu.com/text2audio"
    payload = {
        "tex": text,
        "tok": token,
        "cuid": "blindstick",
        "ctp": 1,
        "lan": "zh",
        "spd": 6,   # 稍快语速
        "pit": 5,
        "vol": 9,
        "per": 1,
        "aue": 6
    }

    print(f"[生成] '{text}' -> {array_name}")
    try:
        resp = requests.post(url, data=payload, timeout=10, verify=False)
        if 'audio' in resp.headers.get('Content-Type', ''):
            data = resp.content[44:]  # 去掉WAV头

            samples = []
            for i in range(0, len(data), 2):
                if i + 1 < len(data):
                    sample = data[i] | (data[i+1] << 8)
                    if sample > 32767:
                        sample -= 65536
                    samples.append(sample)

            # 限制长度（最多2秒）
            max_samples = 32000  # 2秒 @ 16kHz
            if len(samples) > max_samples:
                samples = samples[:max_samples]

            # 生成C数组
            lines = []
            lines.append(f"// 短语音: {text}")
            lines.append(f"// 时长: {len(samples)/16000:.2f}秒")
            lines.append(f"const int16_t {array_name}[] = {{")

            for i in range(0, len(samples), 12):
                row = samples[i:i+12]
                row_str = ", ".join(f"{s:5d}" for s in row)
                lines.append(f"    {row_str},")

            lines.append(f"}};")
            lines.append(f"const int {array_name}_len = sizeof({array_name}) / sizeof({array_name}[0]);")
            lines.append("")

            return "\n".join(lines)

    except Exception as e:
        print(f"[错误] {e}")
    return None

def main():
    print("="*60)
    print("     紧急短语音生成工具")
    print("="*60)
    print()

    all_arrays = []

    for text, name in ALERTS:
        code = generate_short_tts(text, name)
        if code:
            all_arrays.append(code)
            print(f"[成功] {text} -> {name}")
        else:
            print(f"[失败] {text}")
        print()

    if all_arrays:
        # 保存到文件
        with open("alert_audio_arrays.txt", "w", encoding="utf-8") as f:
            f.write("// ==================== 紧急短语音数据 ====================\n\n")
            for arr in all_arrays:
                f.write(arr + "\n")

        print("="*60)
        print("输出文件: alert_audio_arrays.txt")
        print("请复制内容到 esp32_upload.ino 中")
        print("="*60)

if __name__ == "__main__":
    import urllib3
    urllib3.disable_warnings()
    main()
