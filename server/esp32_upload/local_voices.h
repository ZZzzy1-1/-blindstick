// local_voices.h - 本地语音数据头文件
// 由 generate_local_voices.py 自动生成
// 将此文件放在 esp32_upload.ino 同目录

#ifndef LOCAL_VOICES_H
#define LOCAL_VOICES_H

// 语音文件: 前方有障碍物，请向左绕行.wav
extern const int16_t voice_left[];
extern const int voice_left_len;

// 语音文件: 前方有障碍物，请向右绕行.wav
extern const int16_t voice_right[];
extern const int voice_right_len;

// 语音文件: 前方有障碍物，请注意避让.wav
extern const int16_t voice_front[];
extern const int voice_front_len;

// 语音文件: 左前方有障碍物，请向右绕行.wav
extern const int16_t voice_frontleft[];
extern const int voice_frontleft_len;

// 语音文件: 右前方有障碍物，请向左绕行.wav
extern const int16_t voice_frontright[];
extern const int voice_frontright_len;

// 语音文件: 左侧有障碍物，请向右绕行.wav
extern const int16_t voice_leftside[];
extern const int voice_leftside_len;

// 语音文件: 右侧有障碍物，请向左绕行.wav
extern const int16_t voice_rightside[];
extern const int voice_rightside_len;

#endif // LOCAL_VOICES_H
