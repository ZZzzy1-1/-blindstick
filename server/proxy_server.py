#!/usr/bin/env python3
"""
百度API代理服务器 - 流式TTS版本
支持边合成边通过MQTT发送音频给ESP32
运行: python proxy_server.py
"""

from flask import Flask, request, jsonify, Response, send_from_directory
from flask_cors import CORS
import requests
import json
import os
import time
import hashlib
import asyncio
import websockets
import threading
import queue

app = Flask(__name__)
CORS(app, resources={r"/*": {"origins": "*"}})

# ==================== 前端静态文件托管 ====================
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
FRONTEND_DIR = os.path.dirname(BASE_DIR)

@app.route('/')
def index():
    fp = os.path.join(FRONTEND_DIR, 'index.html')
    with open(fp, 'r', encoding='utf-8') as f:
        return f.read()

@app.route('/<path:filename>')
def static_files(filename):
    fp = os.path.join(FRONTEND_DIR, filename)
    if not os.path.exists(fp) or os.path.isdir(fp):
        return "Not Found", 404
    ext = os.path.splitext(fp)[1].lower()
    mime = {
        '.html':'text/html','.css':'text/css','.js':'application/javascript',
        '.json':'application/json','.png':'image/png','.jpg':'image/jpeg',
        '.gif':'image/gif','.svg':'image/svg+xml','.ico':'image/x-icon',
        '.woff':'font/woff','.woff2':'font/woff2','.ttf':'font/ttf',
        '.eot':'application/vnd.ms-fontobject'
    }
    mt = mime.get(ext, 'application/octet-stream')
    with open(fp, 'rb') as f:
        return f.read(), 200, {'Content-Type': mt}

# ==================== 百度API配置 ====================
BAIDU_API_KEY = "Xbxnhkwb2sxtB6HbH5BUTlUG"
BAIDU_SECRET_KEY = "Tw485P2BFGpPu8WeOVP6hy4S1BHqG4ON"
BAIDU_TTS_PER = "4146"  # 发音人

cached_token = {
    "access_token": None,
    "expires_at": 0
}

def get_baidu_token():
    """获取百度Access Token（带缓存）"""
    if cached_token["access_token"] and time.time() < cached_token["expires_at"] - 300:
        return cached_token["access_token"]

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
            cached_token["access_token"] = data["access_token"]
            cached_token["expires_at"] = time.time() + data.get("expires_in", 2592000)
            print(f"[Token] 获取成功，有效期{data.get('expires_in')}秒")
            return data["access_token"]
    except Exception as e:
        print(f"[Token] 获取失败: {e}")

    return None

# ==================== 流式TTS管理器 ====================
class StreamingTTSManager:
    """
    流式TTS管理器
    - 维护与百度TTS的WebSocket连接
    - 边合成边通过回调发送音频数据
    - 支持高优先级打断
    """
    def __init__(self):
        self.current_task = None
        self.session_id = None
        self.is_synthesizing = False
        self.audio_queue = queue.Queue()
        self.priority = 0  # 0=低(导航), 1=高(雷达告警)
        self.lock = threading.Lock()

    def get_priority_name(self, p):
        return "高(雷达)" if p == 2 else "中(对话)" if p == 1 else "低(导航)"

    async def synthesize_streaming(self, text, priority=0, on_audio_chunk=None, on_complete=None):
        """
        流式合成语音
        :param text: 要合成的文本
        :param priority: 优先级 0=低(导航) 1=中(对话) 2=高(雷达告警)
        :param on_audio_chunk: 回调函数(chunk_data, is_last)
        :param on_complete: 完成回调(success, error_msg)
        """
        with self.lock:
            # 如果有更高或同等优先级的正在合成，先停止
            if self.is_synthesizing:
                if priority >= self.priority:
                    print(f"[TTS] 打断当前{self.get_priority_name(self.priority)}优先级，开始{self.get_priority_name(priority)}")
                    # 标记打断，让当前任务退出
                    self.current_task = None
                else:
                    print(f"[TTS] 低优先级{self.get_priority_name(priority)}被忽略，当前正在播放{self.get_priority_name(self.priority)}")
                    if on_complete:
                        on_complete(False, "被高优先级打断")
                    return

            self.current_task = threading.current_thread().ident
            self.priority = priority
            self.is_synthesizing = True
            self.session_id = None

        token = get_baidu_token()
        if not token:
            with self.lock:
                self.is_synthesizing = False
            if on_complete:
                on_complete(False, "无法获取Token")
            return

        ws_url = f"wss://aip.baidubce.com/ws/2.0/speech/publiccloudspeech/v1/tts?access_token={token}&per={BAIDU_TTS_PER}"

        try:
            async with websockets.connect(ws_url) as ws:
                my_task = self.current_task

                # 1. 发送开始合成请求
                start_msg = {
                    "type": "system.start",
                    "payload": {
                        "spd": 5,
                        "pit": 5,
                        "vol": 9,
                        "audio_ctrl": '{"sampling_rate":16000}',
                        "aue": 4  # PCM-16k
                    }
                }
                await ws.send(json.dumps(start_msg))

                # 等待开始响应
                response = await ws.recv()
                resp_data = json.loads(response)
                if resp_data.get("code", -1) != 0:
                    raise Exception(f"开始合成失败: {resp_data.get('message')}")

                self.session_id = resp_data.get("headers", {}).get("session_id")
                print(f"[TTS] 开始合成: '{text[:30]}...' 优先级={self.get_priority_name(priority)} session={self.session_id}")

                # 2. 发送文本
                text_msg = {
                    "type": "text",
                    "payload": {"text": text}
                }
                await ws.send(json.dumps(text_msg))

                # 3. 接收音频数据（流式）
                chunk_count = 0
                total_bytes = 0

                while True:
                    # 检查是否被打断
                    with self.lock:
                        if self.current_task != my_task:
                            print(f"[TTS] 合成被打断，退出")
                            await ws.close()
                            if on_complete:
                                on_complete(False, "被打断")
                            return

                    try:
                        data = await asyncio.wait_for(ws.recv(), timeout=10.0)
                    except asyncio.TimeoutError:
                        print("[TTS] 接收超时")
                        break

                    if isinstance(data, bytes):
                        # 音频数据块
                        chunk_count += 1
                        total_bytes += len(data)
                        if on_audio_chunk:
                            on_audio_chunk(data, False)
                    else:
                        # 文本响应
                        try:
                            msg = json.loads(data)
                            msg_type = msg.get("type", "")

                            if msg_type == "system.error":
                                print(f"[TTS] 错误: {msg}")
                                break
                            elif msg_type == "system.finished":
                                print(f"[TTS] 合成完成，共{chunk_count}块，{total_bytes}字节")
                                break
                            elif "error" in msg.get("message", "").lower():
                                print(f"[TTS] 收到错误: {msg}")
                                break
                        except:
                            pass

                # 4. 发送结束请求
                try:
                    await ws.send(json.dumps({"type": "system.finish"}))
                    # 等待最终响应
                    final_resp = await asyncio.wait_for(ws.recv(), timeout=5.0)
                    final_data = json.loads(final_resp)
                    if final_data.get("code", -1) != 0:
                        print(f"[TTS] 结束响应错误: {final_data}")
                except:
                    pass

                if on_audio_chunk:
                    on_audio_chunk(b'', True)  # 标记结束

                with self.lock:
                    self.is_synthesizing = False

                if on_complete:
                    on_complete(True, None)

        except Exception as e:
            print(f"[TTS] 流式合成异常: {e}")
            with self.lock:
                self.is_synthesizing = False
            if on_complete:
                on_complete(False, str(e))

# 全局TTS管理器
tts_manager = StreamingTTSManager()

# ==================== MQTT客户端（用于发送音频给ESP32）====================
try:
    import paho.mqtt.client as mqtt
    MQTT_AVAILABLE = True
except ImportError:
    MQTT_AVAILABLE = False
    print("[警告] paho-mqtt未安装，无法直接发送音频给ESP32")

class MQTTAudioSender:
    """MQTT音频发送器 - 将流式TTS音频分段发送给ESP32"""
    def __init__(self):
        self.client = None
        self.connected = False
        self.broker = "u72a7838.ala.asia-southeast1.emqxsl.com"
        self.port = 8883
        self.topic_audio = "blindstick/tts/stream"
        self.topic_control = "blindstick/tts/control"
        self.username = "blindstick"
        self.password = "2026"
        self.segment_size = 1024  # 每段音频大小（字节）

    def connect(self):
        if not MQTT_AVAILABLE:
            return False

        try:
            self.client = mqtt.Client(client_id="proxy_server_tts")
            self.client.username_pw_set(self.username, self.password)
            self.client.tls_set()

            def on_connect(client, userdata, flags, rc):
                if rc == 0:
                    self.connected = True
                    print(f"[MQTT] 已连接到 {self.broker}")
                else:
                    print(f"[MQTT] 连接失败，错误码: {rc}")

            def on_disconnect(client, userdata, rc):
                self.connected = False
                print(f"[MQTT] 断开连接")

            self.client.on_connect = on_connect
            self.client.on_disconnect = on_disconnect

            self.client.connect(self.broker, self.port, 60)
            self.client.loop_start()
            return True
        except Exception as e:
            print(f"[MQTT] 连接失败: {e}")
            return False

    def send_audio_stream(self, audio_chunks, priority=0, session_id=None):
        """
        发送音频流给ESP32
        :param audio_chunks: 音频数据块列表或生成器
        :param priority: 优先级
        :param session_id: 会话ID
        """
        if not self.connected or not self.client:
            print("[MQTT] 未连接，无法发送音频")
            return False

        try:
            # 发送开始标记
            control_msg = {
                "type": "stream_start",
                "priority": priority,
                "session_id": session_id or f"tts_{int(time.time())}",
                "format": "pcm_16k"  # PCM 16kHz 16bit
            }
            self.client.publish(self.topic_control, json.dumps(control_msg))
            print(f"[MQTT] 发送 stream_start 优先级={priority}")

            # 发送音频分段
            segment_idx = 0
            for chunk in audio_chunks:
                if not chunk:
                    break

                # 将大块分成小段发送
                for i in range(0, len(chunk), self.segment_size):
                    segment = chunk[i:i+self.segment_size]
                    topic = f"{self.topic_audio}/{segment_idx}"
                    self.client.publish(topic, segment)
                    segment_idx += 1

            # 发送结束标记
            control_msg = {
                "type": "stream_end",
                "segments": segment_idx,
                "session_id": session_id
            }
            self.client.publish(self.topic_control, json.dumps(control_msg))
            print(f"[MQTT] 发送 stream_end，共{segment_idx}段")
            return True

        except Exception as e:
            print(f"[MQTT] 发送音频失败: {e}")
            return False

    def send_interrupt(self, new_priority):
        """发送打断信号"""
        if self.connected and self.client:
            msg = {
                "type": "interrupt",
                "priority": new_priority,
                "timestamp": int(time.time())
            }
            self.client.publish(self.topic_control, json.dumps(msg))
            print(f"[MQTT] 发送打断信号，新优先级={new_priority}")

# 全局MQTT发送器
mqtt_sender = MQTTAudioSender()

# ==================== API接口 ====================

@app.route('/api/tts/stream', methods=['POST'])
def tts_stream():
    """
    流式TTS接口
    请求: {"text": "要合成的文本", "priority": 0}
    priority: 0=低(导航), 1=中(对话), 2=高(雷达告警)
    """
    try:
        data = request.get_json(force=True, silent=True) or {}
        text = data.get('text', '')
        priority = data.get('priority', 0)

        if not text:
            return jsonify({"error": "缺少text参数"}), 400

        print(f"[API] 流式TTS请求: '{text[:30]}...' 优先级={priority}")

        # 收集音频数据（用于返回给前端）
        audio_buffer = []
        audio_ready = threading.Event()

        def on_audio_chunk(chunk, is_last):
            if not is_last and chunk:
                audio_buffer.append(chunk)

        def on_complete(success, error):
            audio_ready.set()

        # 启动流式合成（异步）
        def run_tts():
            asyncio.run(tts_manager.synthesize_streaming(
                text, priority, on_audio_chunk, on_complete
            ))

        tts_thread = threading.Thread(target=run_tts)
        tts_thread.start()

        # 等待完成（最多30秒）
        audio_ready.wait(timeout=30)

        # 合并音频数据返回
        if audio_buffer:
            full_audio = b''.join(audio_buffer)
            return Response(full_audio, mimetype='audio/pcm')
        else:
            return jsonify({"error": "合成失败或无音频数据"}), 500

    except Exception as e:
        print(f"[TTS Stream] 异常: {e}")
        return jsonify({"error": str(e)}), 500


@app.route('/api/tts/push', methods=['POST'])
def tts_push():
    """
    推送TTS到ESP32播放（流式合成 + MQTT推送）
    请求: {"text": "要合成的文本", "priority": 0}
    """
    try:
        data = request.get_json(force=True, silent=True) or {}
        text = data.get('text', '')
        priority = data.get('priority', 0)

        if not text:
            return jsonify({"error": "缺少text参数"}), 400

        print(f"[API] 推送TTS到ESP32: '{text[:30]}...' 优先级={priority}")

        # 确保MQTT已连接
        if not mqtt_sender.connected:
            mqtt_sender.connect()
            time.sleep(0.5)  # 等待连接

        # 收集音频数据
        audio_chunks = []
        completed = threading.Event()
        success = [False]

        def on_audio_chunk(chunk, is_last):
            if not is_last and chunk:
                audio_chunks.append(chunk)

        def on_complete(ok, error):
            success[0] = ok
            completed.set()

        # 启动流式合成
        def run_tts():
            asyncio.run(tts_manager.synthesize_streaming(
                text, priority, on_audio_chunk, on_complete
            ))

        tts_thread = threading.Thread(target=run_tts)
        tts_thread.start()

        # 等待有足够数据或完成
        chunk_count = 0
        while not completed.is_set() or chunk_count < len(audio_chunks):
            # 有新数据就立即发送
            while chunk_count < len(audio_chunks):
                chunk = audio_chunks[chunk_count]
                mqtt_sender.send_audio_stream([chunk], priority)
                chunk_count += 1
            time.sleep(0.01)  # 10ms检查一次

        return jsonify({
            "status": "ok" if success[0] else "error",
            "message": "已推送到ESP32" if success[0] else "合成失败"
        })

    except Exception as e:
        print(f"[TTS Push] 异常: {e}")
        return jsonify({"error": str(e)}), 500


@app.route('/api/tts/interrupt', methods=['POST'])
def tts_interrupt():
    """打断当前播放"""
    try:
        data = request.get_json(force=True, silent=True) or {}
        priority = data.get('priority', 2)

        mqtt_sender.send_interrupt(priority)

        return jsonify({"status": "ok", "message": "已发送打断信号"})
    except Exception as e:
        return jsonify({"error": str(e)}), 500


# ==================== 原有接口保持兼容 ====================

@app.route('/api/tts', methods=['POST'])
def text_to_speech():
    """兼容旧版短文本合成接口（改为流式实现）"""
    return tts_stream()


@app.route('/api/asr', methods=['POST'])
def speech_to_text():
    """语音识别接口"""
    try:
        pcm_data = request.data

        if len(pcm_data) < 1000:
            return jsonify({"error": "音频数据太短"}), 400

        print(f"[ASR] 收到音频: {len(pcm_data)} 字节")

        token = get_baidu_token()
        if not token:
            return jsonify({"error": "无法获取Token"}), 500

        import base64
        url = "https://vop.baidu.com/server_api"
        payload = {
            "format": "pcm",
            "rate": 16000,
            "channel": 1,
            "cuid": "blindstick_proxy",
            "token": token,
            "dev_pid": 1537,
            "speech": base64.b64encode(pcm_data).decode('utf-8'),
            "len": len(pcm_data)
        }

        resp = requests.post(url, json=payload, timeout=30, verify=False)
        result = resp.json()

        if result.get("err_no") == 0:
            text = result.get("result", [""])[0]
            print(f"[ASR] 识别结果: {text}")
            return jsonify({"text": text})
        else:
            print(f"[ASR] 错误: {result.get('err_msg')}")
            return jsonify({"error": result.get("err_msg")}), 400

    except Exception as e:
        print(f"[ASR] 异常: {e}")
        return jsonify({"error": str(e)}), 500


@app.route('/health', methods=['GET'])
def health_check():
    """健康检查"""
    return jsonify({
        "status": "ok",
        "mqtt_connected": mqtt_sender.connected,
        "tts_synthesizing": tts_manager.is_synthesizing
    })


# ==================== 用户配置接口 ====================
CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'user_config.json')

def _load_config():
    try:
        with open(CONFIG_FILE, 'r', encoding='utf-8') as f:
            return json.load(f)
    except Exception:
        return {"home_city": "黄石市"}

def _save_config(data):
    with open(CONFIG_FILE, 'w', encoding='utf-8') as f:
        json.dump(data, f, ensure_ascii=False, indent=2)

@app.route('/api/config', methods=['GET'])
def get_config():
    try:
        cfg = _load_config()
        return jsonify({"status": "ok", "home_city": cfg.get("home_city", "黄石市")})
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/config', methods=['POST'])
def set_config():
    try:
        data = request.get_json()
        cfg = _load_config()
        if "home_city" in data:
            cfg["home_city"] = data["home_city"]
        _save_config(cfg)
        print(f"[配置] 已保存: home_city={cfg['home_city']}")
        return jsonify({"status": "ok", "message": "配置已保存", "home_city": cfg["home_city"]})
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/navigation/stop', methods=['POST'])
def stop_navigation():
    print("[导航] 收到停止导航请求")
    return jsonify({"status": "ok", "message": "导航已停止"})

@app.route('/api/nav_steps', methods=['GET', 'POST'])
def get_nav_steps():
    print("[导航] 收到路线请求")
    return jsonify({
        "status": "ok",
        "steps": [
            "前方直行100米",
            "左转进入主干道",
            "继续直行200米",
            "右转到达目的地"
        ]
    })


# ==================== 启动 ====================
if __name__ == '__main__':
    import urllib3
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

    # 启动时连接MQTT
    print("[启动] 正在连接MQTT...")
    mqtt_sender.connect()
    time.sleep(1)

    port = int(os.environ.get('PORT', 8090))

    print("=" * 50)
    print("百度API代理服务器 - 流式TTS版本")
    print("=" * 50)
    print("API接口:")
    print("  POST /api/tts/stream      - 流式TTS（返回音频）")
    print("  POST /api/tts/push        - 推送TTS到ESP32")
    print("  POST /api/tts/interrupt   - 打断当前播放")
    print("  POST /api/asr             - 语音识别")
    print("  GET  /health              - 健康检查")
    print("=" * 50)
    print(f"运行在: http://0.0.0.0:{port}")
    print("=" * 50)

    app.run(host='0.0.0.0', port=port, debug=False)
