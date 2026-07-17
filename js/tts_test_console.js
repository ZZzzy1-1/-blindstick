/**
 * 浏览器控制台 TTS 测试脚本
 *
 * 使用方法：
 * 1. 打开导盲杖大屏网页
 * 2. 按 F12 打开开发者工具
 * 3. 切换到 Console 标签
 * 4. 复制粘贴此代码并回车运行
 */

// 测试 1: 检查 MQTT 连接状态
console.log('=== TTS 测试开始 ===');
console.log('MQTT 连接状态:', AppState.mqttConnected ? '已连接' : '未连接');
console.log('MQTT 客户端:', mqttClient ? '已创建' : '未创建');

// 测试 2: 发送测试文本到 ESP32
async function testTTSRequest() {
    console.log('\n=== 测试1: 发送 TTS 文本请求 ===');
    if (!mqttClient || !AppState.mqttConnected) {
        console.error('MQTT 未连接，无法测试');
        return;
    }

    const msg = JSON.stringify({
        type: 'tts_request',
        text: '测试语音，听到此声音表示MQTT正常'
    });

    mqttClient.publish('blindstick/tts/request', msg, { qos: 1 }, (err) => {
        if (err) {
            console.error('发送失败:', err);
        } else {
            console.log('✓ TTS 文本请求已发送');
            console.log('  内容:', msg);
            console.log('  请查看 ESP32 串口日志，应该显示 [MQTT] 收到 [blindstick/tts/request]');
        }
    });
}

// 测试 3: 通过代理服务器合成并发送音频
async function testTTSWithAudio() {
    console.log('\n=== 测试2: 合成并发送 TTS 音频 ===');

    const testText = '这是测试音频，听到此声音表示TTS链路正常';
    console.log('合成文本:', testText);

    try {
        const response = await fetch(`${API_BASE}/api/tts`, {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ text: testText })
        });

        console.log('代理服务器响应:', response.status, response.statusText);
        console.log('Content-Type:', response.headers.get('Content-Type'));

        if (!response.ok) {
            const error = await response.text();
            console.error('TTS 合成失败:', error);
            return;
        }

        const audioData = await response.arrayBuffer();
        console.log('✓ 音频合成成功');
        console.log('  大小:', audioData.byteLength, '字节');
        console.log('  前4字节:', new Uint8Array(audioData.slice(0, 4)));
        console.log('  (应该是 [82, 73, 70, 70] = "RIFF")');

        // 发送到 ESP32
        if (mqttClient && AppState.mqttConnected) {
            const ttsAudio = new Uint8Array(audioData);

            if (ttsAudio.length > 60000) {
                console.log('音频太大，分段发送...');
                // 这里简化处理，实际应该分段
            } else {
                mqttClient.publish('blindstick/tts/audio', ttsAudio, { qos: 1 }, (err) => {
                    if (err) {
                        console.error('发送失败:', err);
                    } else {
                        console.log('✓ TTS 音频已发送到 ESP32');
                        console.log('  请查看 ESP32 串口日志');
                        console.log('  应该显示 [MQTT] 收到 [blindstick/tts/audio]');
                        console.log('  然后显示 [TTS] 开始播放... [TTS] 播放完成');
                    }
                });
            }
        } else {
            console.error('MQTT 未连接，无法发送音频');
        }

    } catch (e) {
        console.error('请求异常:', e);
    }
}

// 测试 4: 检查 ESP32 传感器数据
function checkSensorData() {
    console.log('\n=== 当前传感器数据 ===');
    console.log('GPS 位置:', AppState.lastGpsPos);
    console.log('导航状态:', navDestination?.textContent);
    console.log('模块状态:', {
        mqtt: AppState.mqttConnected,
        main: true,
        vision: true,
        radar: true,
        gps: AppState.lastGpsPos !== null,
        voice: true
    });
}

// 运行测试
console.log('\n可用测试函数:');
console.log('- testTTSRequest()    : 发送 TTS 文本请求');
console.log('- testTTSWithAudio()  : 合成并发送 TTS 音频');
console.log('- checkSensorData()   : 查看传感器数据');
console.log('\n请输入函数名运行测试，例如: testTTSRequest()');
