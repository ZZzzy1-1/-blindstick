#!/usr/bin/env python3
"""
开机语音生成工具
生成"系统启动成功"的语音文件并转换为C数组
"""

import requests
import json
import hashlib
import time
import os

# 百度API配置（使用你项目中已有的key）
BAIDU_API_KEY = "Xbxnhkwb2sxtB6HbH5BUTlUG"
BAIDU_SECRET_KEY = "Tw485P2BFGpPu8WeOVP6hy4S1BHqG4ON"

def get_baidu_token():
    """获取百度Access Token"""
    url = "https://aip.baidubce.com/oauth/2.0/token"
    params = {
        "grant_type": "client_credentials",
        "client_id": BAIDU_API_KEY,
        "client_secret": BAIDU_SECRET_KEY
    }
    try:
        resp = requests.post(url, params=params, timeout=10, verify=False)
        data = resp.json()
        if "access_token" in data:
            print(f"[Token] 获取成功，有效期{data.get('expires_in')}秒")
            return data["access_token"]
    except Exception as e:
        print(f"[Token] 错误: {e}")
    return None

def generate_tts(text="系统启动成功", filename="startup.wav"):
    """生成TTS音频并保存"""
    token = get_baidu_token()
    if not token:
        print("[TTS] 无法获取Token")
        return None

    url = "https://tsn.baidu.com/text2audio"
    payload = {
        "tex": text,
        "tok": token,
        "cuid": "blindstick_generator",
        "ctp": 1,
        "lan": "zh",
        "spd": 5,  # 语速
        "pit": 5,  # 音调
        "vol": 9,  # 音量
        "per": 1,  # 发音人（1=女声，0=男声）
        "aue": 6   # 输出格式 6=wav
    }

    print(f"[TTS] 正在合成语音: '{text}'")
    try:
        resp = requests.post(url, data=payload, timeout=15, verify=False)

        if 'audio' in resp.headers.get('Content-Type', ''):
            # 保存音频文件
            with open(filename, 'wb') as f:
                f.write(resp.content)
            print(f"[TTS] 合成成功! 文件: {filename}, 大小: {len(resp.content)} 字节")
            return filename
        else:
            print(f"[TTS] 合成失败: {resp.text[:200]}")
    except Exception as e:
        print(f"[TTS] 请求错误: {e}")
    return None

def wav_to_c_array(wav_file, array_name="startup_audio"):
    """将WAV文件转换为C语言数组（去掉WAV头）"""
    try:
        with open(wav_file, 'rb') as f:
            data = f.read()

        # 检查WAV头
        if data[:4] == b'RIFF':
            print(f"[转换] 检测到WAV头，去掉前44字节")
            pcm_data = data[44:]  # 去掉WAV头
        else:
            pcm_data = data

        # 转换为16位有符号整数数组
        samples = []
        for i in range(0, len(pcm_data), 2):
            if i + 1 < len(pcm_data):
                sample = pcm_data[i] | (pcm_data[i+1] << 8)
                if sample > 32767:
                    sample -= 65536
                samples.append(sample)

        # 生成C数组代码
        lines = []
        lines.append(f"// 开机语音: 系统启动成功")
        lines.append(f"// 采样率: 16000Hz, 16bit, PCM")
        lines.append(f"// 原始文件: {wav_file}")
        lines.append(f"// 数据大小: {len(pcm_data)} 字节, {len(samples)} 采样点")
        lines.append(f"const int16_t {array_name}[] = {{")

        # 每行16个数据
        for i in range(0, len(samples), 16):
            row = samples[i:i+16]
            row_str = ", ".join(f"{s:6d}" for s in row)
            lines.append(f"    {row_str},")

        lines.append(f"}};")
        lines.append(f"const int {array_name}_len = sizeof({array_name}) / sizeof({array_name}[0]);")
        lines.append("")

        c_code = "\n".join(lines)

        # 保存到文件
        output_file = f"{array_name}.h"
        with open(output_file, 'w', encoding='utf-8') as f:
            f.write(c_code)

        print(f"[转换] C数组已保存到: {output_file}")
        print(f"[转换] 数组长度: {len(samples)} 个采样点")
        print(f"[转换] 播放时长: {len(samples)/16000:.2f} 秒")

        # 同时打印到控制台
        print("\n========== 复制以下内容到 esp32_upload.ino ==========\n")
        print(c_code)
        print("\n====================================================\n")

        return c_code

    except Exception as e:
        print(f"[转换] 错误: {e}")
        import traceback
        traceback.print_exc()
        return None

def main():
    print("="*60)
    print("       开机语音生成工具")
    print("="*60)
    print()

    # 生成语音
    wav_file = generate_tts("系统启动成功", "startup.wav")

    if wav_file and os.path.exists(wav_file):
        print()
        # 转换为C数组
        wav_to_c_array(wav_file, "startup_audio")

        print()
        print("="*60)
        print("使用说明:")
        print("1. 打开生成的 startup_audio.h 文件")
        print("2. 复制内容替换 esp32_upload.ino 中的 startup_audio 数组")
        print("3. 重新上传ESP32代码")
        print("="*60)
    else:
        print("[错误] 语音生成失败")

if __name__ == "__main__":
    import urllib3
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)
    main()
