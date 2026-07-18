#!/usr/bin/env python3
"""
Baidu API Proxy Server - TTS Version
Supports streaming TTS and MQTT audio delivery to ESP32
Run: python proxy_server.py
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

# ==================== Config ====================
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
FRONTEND_DIR = os.path.dirname(BASE_DIR)
AUDIO_CACHE_DIR = os.path.join(BASE_DIR, 'audio_cache')
os.makedirs(AUDIO_CACHE_DIR, exist_ok=True)

app = Flask(__name__)
CORS(app, resources={r"/*": {"origins": "*"}})

# ==================== Baidu API Config ====================
BAIDU_API_KEY = "Xbxnhkwb2sxtB6HbH5BUTlUG"
BAIDU_SECRET_KEY = "Tw485P2BFGpPu8WeOVP6hy4S1BHqG4ON"
BAIDU_TTS_PER = "4146"

cached_token = {"access_token": None, "expires_at": 0}

def get_baidu_token():
    if cached_token["access_token"] and time.time() < cached_token["expires_at"] - 300:
        return cached_token["access_token"]
    url = "https://aip.baidubce.com/oauth/2.0/token"
    params = {"grant_type": "client_credentials", "client_id": BAIDU_API_KEY, "client_secret": BAIDU_SECRET_KEY}
    try:
        resp = requests.post(url, params=params, timeout=10, verify=False)
        data = resp.json()
        if "access_token" in data:
            cached_token["access_token"] = data["access_token"]
            cached_token["expires_at"] = time.time() + data.get("expires_in", 2592000)
            print(f"[Token] Got token, expires in {data.get('expires_in')}s")
            return data["access_token"]
    except Exception as e:
        print(f"[Token] Error: {e}")
    return None

# ==================== Streaming TTS Manager ====================
class StreamingTTSManager:
    def __init__(self):
        self.current_task = None
        self.session_id = None
        self.is_synthesizing = False
        self.audio_queue = queue.Queue()
        self.priority = 0
        self.lock = threading.Lock()

    def get_priority_name(self, p):
        return "HIGH(Radar)" if p == 2 else "NORMAL(Chat)" if p == 1 else "LOW(Nav)"

    async def synthesize_streaming(self, text, priority=0, on_audio_chunk=None, on_complete=None):
        with self.lock:
            if self.is_synthesizing:
                if priority >= self.priority:
                    print(f"[TTS] Interrupting {self.get_priority_name(self.priority)}, starting {self.get_priority_name(priority)}")
                    self.current_task = None
                else:
                    print(f"[TTS] Ignoring low priority {self.get_priority_name(priority)}")
                    if on_complete:
                        on_complete(False, "Interrupted by high priority")
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
                on_complete(False, "Cannot get token")
            return

        ws_url = f"wss://aip.baidubce.com/ws/2.0/speech/publiccloudspeech/v1/tts?access_token={token}&per={BAIDU_TTS_PER}"
        try:
            async with websockets.connect(ws_url) as ws:
                my_task = self.current_task
                start_msg = {
                    "type": "system.start",
                    "payload": {"spd": 5, "pit": 5, "vol": 9, "audio_ctrl": '{"sampling_rate":16000}', "aue": 4}
                }
                await ws.send(json.dumps(start_msg))
                response = await ws.recv()
                resp_data = json.loads(response)
                if resp_data.get("code", -1) != 0:
                    raise Exception(f"Start failed: {resp_data.get('message')}")
                self.session_id = resp_data.get("headers", {}).get("session_id")
                print(f"[TTS] Starting '{text[:30]}...' priority={self.get_priority_name(priority)} session={self.session_id}")

                text_msg = {"type": "text", "payload": {"text": text}}
                await ws.send(json.dumps(text_msg))

                chunk_count = 0
                total_bytes = 0
                while True:
                    with self.lock:
                        if self.current_task != my_task:
                            print("[TTS] Synthesis interrupted")
                            await ws.close()
                            if on_complete:
                                on_complete(False, "Interrupted")
                            return
                    try:
                        data = await asyncio.wait_for(ws.recv(), timeout=10.0)
                    except asyncio.TimeoutError:
                        print("[TTS] Receive timeout")
                        break
                    if isinstance(data, bytes):
                        chunk_count += 1
                        total_bytes += len(data)
                        if on_audio_chunk:
                            on_audio_chunk(data, False)
                    else:
                        try:
                            msg = json.loads(data)
                            msg_type = msg.get("type", "")
                            if msg_type == "system.error":
                                print(f"[TTS] Error: {msg}")
                                break
                            elif msg_type == "system.finished":
                                print(f"[TTS] Finished, {chunk_count} chunks, {total_bytes} bytes")
                                break
                            elif "error" in msg.get("message", "").lower():
                                print(f"[TTS] Error in message: {msg}")
                                break
                        except:
                            pass
                try:
                    await ws.send(json.dumps({"type": "system.finish"}))
                    final_resp = await asyncio.wait_for(ws.recv(), timeout=5.0)
                except:
                    pass
                if on_audio_chunk:
                    on_audio_chunk(b'', True)
                with self.lock:
                    self.is_synthesizing = False
                if on_complete:
                    on_complete(True, None)
        except Exception as e:
            print(f"[TTS] Streaming error: {e}")
            with self.lock:
                self.is_synthesizing = False
            if on_complete:
                on_complete(False, str(e))

tts_manager = StreamingTTSManager()

# ==================== MQTT Client ====================
try:
    import paho.mqtt.client as mqtt
    MQTT_AVAILABLE = True
    # 检查是否有 CallbackAPIVersion (paho-mqtt >= 2.0)
    try:
        from paho.mqtt.enums import CallbackAPIVersion
        MQTT_V2 = True
    except ImportError:
        MQTT_V2 = False
except ImportError:
    MQTT_AVAILABLE = False
    MQTT_V2 = False
    print("[Warning] paho-mqtt not installed")

class MQTTAudioSender:
    def __init__(self):
        self.client = None
        self.connected = False
        self.broker = "u72a7838.ala.asia-southeast1.emqxsl.com"
        self.port = 8883
        self.topic_control = "blindstick/tts/control"
        self.username = "blindstick"
        self.password = "2026"
        self.connect_retry_count = 0
        self.max_retries = 5

    def on_message(self, client, userdata, msg):
        """处理接收到的MQTT消息 - 使用线程池异步处理避免阻塞"""
        try:
            topic = msg.topic
            payload = msg.payload.decode('utf-8')
            print(f"[MQTT] Received on {topic}: {payload[:80]}...")

            if topic == "blindstick/tts/request":
                # 快速解析，异步处理TTS
                data = json.loads(payload)
                text = data.get("text", "")
                priority = data.get("priority", 0)

                if text:
                    print(f"[MQTT] TTS快速接收: '{text[:30]}...' 优先级={priority}")
                    # 使用线程异步处理，不阻塞MQTT回调
                    import threading
                    t = threading.Thread(
                        target=self.handle_tts_request,
                        args=(text, priority),
                        daemon=True
                    )
                    t.start()

        except Exception as e:
            print(f"[MQTT] Message handling error: {e}")

    def handle_tts_request(self, text, priority=0):
        """处理TTS请求：调用百度TTS并推送URL到ESP32"""
        try:
            token = get_baidu_token()
            if not token:
                print("[TTS] Cannot get token")
                return

            url = "https://tsn.baidu.com/text2audio"
            payload = {
                "tex": text,
                "tok": token,
                "cuid": "blindstick_proxy",
                "ctp": 1,
                "lan": "zh",
                "spd": 5,
                "pit": 5,
                "vol": 9,
                "per": 1,
                "aue": 6
            }

            resp = requests.post(url, data=payload, timeout=15, verify=False)

            if 'audio' in resp.headers.get('Content-Type', ''):
                audio_data = resp.content
                print(f"[TTS] Synthesis success: {len(audio_data)} bytes")

                # 保存音频文件
                timestamp = int(time.time())
                file_hash = hashlib.md5(text.encode()).hexdigest()[:8]
                filename = f"tts_{timestamp}_{file_hash}.wav"
                filepath = os.path.join(AUDIO_CACHE_DIR, filename)
                os.makedirs(AUDIO_CACHE_DIR, exist_ok=True)

                with open(filepath, 'wb') as f:
                    f.write(audio_data)

                # 生成公网URL并推送
                full_url = f"https://blindstick-4.onrender.com/audio/{filename}"
                url_payload = json.dumps({
                    "type": "tts_url",
                    "url": full_url,
                    "text": text[:30],
                    "priority": priority
                })

                self.client.publish("blindstick/tts/url", url_payload)
                print(f"[MQTT] TTS URL pushed: {full_url}")
            else:
                print(f"[TTS] Synthesis failed: {resp.text[:200]}")

        except Exception as e:
            print(f"[TTS] Handle request error: {e}")
            import traceback
            traceback.print_exc()

    def connect(self):
        if not MQTT_AVAILABLE:
            print("[MQTT] paho-mqtt not available")
            return False

        if self.connected:
            return True

        try:
            # 使用新版 API (paho-mqtt >= 2.0) 或旧版 API
            if MQTT_V2:
                self.client = mqtt.Client(
                    callback_api_version=CallbackAPIVersion.VERSION2,
                    client_id="proxy_server_tts"
                )
            else:
                self.client = mqtt.Client(client_id="proxy_server_tts")

            self.client.username_pw_set(self.username, self.password)

            # 设置TLS，但禁用证书验证（Render环境可能缺少CA证书）
            try:
                self.client.tls_set(cert_reqs=0)
                self.client.tls_insecure_set(True)
            except Exception as e:
                print(f"[MQTT] TLS setup warning: {e}")

            def on_connect(client, userdata, flags, rc, properties=None):
                # 兼容新旧版本回调
                if isinstance(rc, int):
                    reason_code = rc
                else:
                    reason_code = rc.value if hasattr(rc, 'value') else 0

                if reason_code == 0:
                    self.connected = True
                    self.connect_retry_count = 0
                    print(f"[MQTT] Connected to {self.broker}")
                    # 订阅TTS请求主题
                    client.subscribe("blindstick/tts/request")
                    print("[MQTT] Subscribed to blindstick/tts/request")
                else:
                    print(f"[MQTT] Connection failed, code: {reason_code}")

            def on_disconnect(client, userdata, disconnect_flags=None, rc=None, properties=None):
                """处理断开连接，兼容新旧版本"""
                self.connected = False
                # 处理不同版本的参数
                if rc is not None:
                    print(f"[MQTT] Disconnected, code: {rc}")
                else:
                    print("[MQTT] Disconnected")

            def on_connect_fail(client, userdata):
                print("[MQTT] Connection failed (on_connect_fail)")
                self.connected = False

            self.client.on_connect = on_connect
            self.client.on_disconnect = on_disconnect
            self.client.on_connect_fail = on_connect_fail
            self.client.on_message = self.on_message

            # 设置连接超时
            self.client.connect_timeout = 15

            print(f"[MQTT] Connecting to {self.broker}:{self.port}...")
            self.client.connect(self.broker, self.port, keepalive=60)
            self.client.loop_start()
            return True

        except Exception as e:
            print(f"[MQTT] Connection error: {e}")
            import traceback
            traceback.print_exc()
            self.connect_retry_count += 1
            if self.connect_retry_count < self.max_retries:
                print(f"[MQTT] Will retry ({self.connect_retry_count}/{self.max_retries})")
            return False

    def reconnect_if_needed(self):
        """检查连接状态，如需要则重连"""
        if not self.connected and self.connect_retry_count < self.max_retries:
            print("[MQTT] Reconnecting...")
            return self.connect()
        return self.connected

mqtt_sender = MQTTAudioSender()

# ==================== Static Files ====================
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
    mime = {'.html':'text/html','.css':'text/css','.js':'application/javascript','.json':'application/json','.png':'image/png','.jpg':'image/jpeg','.gif':'image/gif','.svg':'image/svg+xml','.ico':'image/x-icon','.woff':'font/woff','.woff2':'font/woff2','.ttf':'font/ttf','.eot':'application/vnd.ms-fontobject'}
    mt = mime.get(ext, 'application/octet-stream')
    with open(fp, 'rb') as f:
        return f.read(), 200, {'Content-Type': mt}

@app.route('/audio/<filename>')
def serve_audio(filename):
    """提供TTS音频文件下载（ESP32通过URL下载）"""
    if '..' in filename or filename.startswith('/'):
        return "Invalid filename", 400
    filepath = os.path.join(AUDIO_CACHE_DIR, filename)
    if not os.path.exists(filepath):
        return "Audio file not found", 404
    return send_from_directory(AUDIO_CACHE_DIR, filename, mimetype='audio/wav')

@app.route('/health', methods=['GET'])
def health_check():
    """健康检查接口"""
    return jsonify({
        "status": "ok",
        "mqtt_connected": mqtt_sender.connected,
        "mode": "mqtt_only"
    })

# ==================== Main ====================
if __name__ == '__main__':
    import urllib3
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

    port = int(os.environ.get('PORT', 8090))
    print("=" * 50)
    print("Baidu API Proxy Server - TTS Version (MQTT Only)")
    print("=" * 50)
    print(f"Running at: http://0.0.0.0:{port}")
    print("=" * 50)

    # 启动 MQTT 连接（在后台重试）
    print("[Startup] Starting MQTT connection...")
    def mqtt_connect_loop():
        while not mqtt_sender.connected and mqtt_sender.connect_retry_count < mqtt_sender.max_retries:
            mqtt_sender.connect()
            if not mqtt_sender.connected:
                time.sleep(5)
        if mqtt_sender.connected:
            print("[MQTT] Connection established successfully")
        else:
            print("[MQTT] Failed to connect after maximum retries")
            print("[MQTT] TTS service will not work without MQTT connection")

    # 在后台线程启动 MQTT 连接
    import threading
    mqtt_thread = threading.Thread(target=mqtt_connect_loop, daemon=True)
    mqtt_thread.start()

    # 启动 Flask 服务
    app.run(host='0.0.0.0', port=port, debug=False)
