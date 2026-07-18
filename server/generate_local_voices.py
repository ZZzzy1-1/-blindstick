#!/usr/bin/env python3
"""
生成本地完整语音数据头文件 local_voices.h
将生成的文件放在 esp32_upload.ino 同目录
"""

import requests
import urllib3
urllib3.disable_warnings()

BAIDU_API_KEY = "Xbxnhkwb2sxtB6HbH5BUTlUG"
BAIDU_SECRET_KEY = "Tw485P2BFGpPu8WeOVP6hy4S1BHqG4ON"

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
    url = "https://aip.baidubce.com/oauth/2.0/token"
    params = {"grant_type": "client_credentials", "client_id": BAIDU_API_KEY, "client_secret": BAIDU_SECRET_KEY}
    try:
        resp = requests.post(url, params=params, timeout=10, verify=False)
        return resp.json().get("access_token")
    except:
        return None

def generate_voice(text, array_name, token):
    url = "https://tsn.baidu.com/text2audio"
    payload = {"tex": text, "tok": token, "cuid": "blindstick", "ctp": 1, "lan": "zh", "spd": 5, "pit": 5, "vol": 9, "per": 1, "aue": 6}
    print(f"[生成] {text}")
    try:
        resp = requests.post(url, data=payload, timeout=20, verify=False)
        if "audio" not in resp.headers.get("Content-Type", ""):
            return None
        data = resp.content[44:]
        samples = []
        for i in range(0, len(data), 2):
            if i + 1 < len(data):
                s = data[i] | (data[i+1] << 8)
                if s > 32767: s -= 65536
                samples.append(s)
        lines = [f"// {text}", f"const int16_t {array_name}[] = {{"]
        for i in range(0, len(samples), 6):
            row = samples[i:i+6]
            lines.append("    " + ", ".join(f"{s:6d}" for s in row) + ",")
        lines.append(f"}};")
        lines.append(f"const int {array_name}_len = sizeof({array_name}) / sizeof({array_name}[0]);\n")
        return "\n".join(lines), len(samples)
    except Exception as e:
        print(f"[错误] {e}")
        return None, 0

def main():
    print("="*60)
    print("生成本地语音数据头文件")
    print("="*60)
    token = get_token()
    if not token:
        print("Token获取失败")
        return
    all_voices = []
    total_samples = 0
    for text, name in VOICES:
        code, num = generate_voice(text, name, token)
        if code:
            all_voices.append(code)
            total_samples += num
            print(f"[成功] {name}: {num}采样点")
    if not all_voices:
        print("生成失败")
        return
    # 生成头文件内容
    header = ["#ifndef LOCAL_VOICES_H", "#define LOCAL_VOICES_H", "", "#include <Arduino.h>", ""]
    for v in all_voices:
        header.append(v)
    header.append("#endif // LOCAL_VOICES_H")
    output = "local_voices.h"
    with open(output, "w", encoding="utf-8") as f:
        f.write("\n".join(header))
    print(f"\n[完成] 已保存到: {output}")
    print(f"总采样点: {total_samples} ({total_samples/16000:.1f}秒)")
    print(f"预估Flash占用: {total_samples*2/1024:.1f}KB")

if __name__ == "__main__":
    main()
