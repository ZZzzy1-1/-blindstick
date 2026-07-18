import re

# 读取 startup_audio.h
with open('server/startup_audio.h', 'r', encoding='utf-8') as f:
    new_audio_data = f.read()

# 读取 esp32_upload.ino
with open('server/esp32_upload/esp32_upload.ino', 'r', encoding='utf-8') as f:
    content = f.read()

# 查找并替换 startup_audio 数组
# 匹配从 "// 开机语音" 或 "// 提示音" 开始到 "};" 之后的 const int startup_audio_len
pattern = r'(// 开机语音.*|// 提示音.*)const int16_t startup_audio\[\] = \{.*?\};\s*const int startup_audio_len = sizeof\(startup_audio\) / sizeof\(startup_audio\[0\]\);'

replacement = new_audio_data.strip()

# 使用 re.DOTALL 让 . 匹配换行符
new_content = re.sub(pattern, replacement, content, flags=re.DOTALL)

# 保存修改后的文件
with open('server/esp32_upload/esp32_upload.ino', 'w', encoding='utf-8') as f:
    f.write(new_content)

print("✅ 已成功替换 startup_audio 数组！")
print(f"新数组大小: {len(new_audio_data)} 字符")
