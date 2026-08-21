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

def _baidu_asr(base64_speech, length):
    """调用百度语音识别（HTTP路由与MQTT通道共用）
    返回: (0, 识别文本) 或 (err_no, 错误描述)
    """
    try:
        token = get_baidu_token()
        if not token:
            return (-3, "cannot get baidu token")
        payload = {
            "format": "pcm",
            "rate": 16000,
            "channel": 1,
            "cuid": "blindstick_proxy",
            "token": token,
            "dev_pid": 1537,
            "speech": base64_speech,
            "len": int(length),
        }
        resp = requests.post("https://vop.baidu.com/server_api", json=payload, timeout=20, verify=False)
        rdata = resp.json()
        print(f"[ASR] Baidu err_no={rdata.get('err_no')} result={rdata.get('result')}")
        if rdata.get("err_no") == 0 and rdata.get("result"):
            return (0, rdata["result"][0])
        return (rdata.get("err_no", -4), rdata.get("err_msg", "asr error"))
    except Exception as e:
        print(f"[ASR] Error: {e}")
        import traceback
        traceback.print_exc()
        return (-5, str(e))

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
        # 【修复】移除最大重试次数限制，改为无限重试
        self.max_retries = 99999
        # 【修复】客户端ID加随机后缀，避免与其他实例冲突被踢下线
        self.client_id = "proxy_server_tts_" + str(int(time.time() * 1000))[-8:]
        # 【ASR over MQTT】分片缓冲状态（绕开热点对443的封锁，ASR音频改走MQTT上传）
        self.asr_upload_buf = bytearray()
        self.asr_upload_active = False

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
                    print(f"[MQTT] TTS请求来自: {msg.retain and '保留消息' or '实时消息'}, 内容: '{text[:30]}...'")
                    # 【修复】移除开机语音跳过逻辑，让所有TTS请求都能正常合成
                    # 开机语音"系统启动成功..."也需要通过代理合成播放
                    # 使用线程异步处理，不阻塞MQTT回调
                    import threading
                    t = threading.Thread(
                        target=self.handle_tts_request,
                        args=(text, priority),
                        daemon=True
                    )
                    t.start()

            elif topic == "blindstick/asr/upload":
                # ESP32通过MQTT分片上传的ASR音频
                self._handle_asr_upload(payload)

            elif topic == "blindstick/places/request":
                # ESP32地点搜索请求（走MQTT绕开443，异步调百度地图）
                threading.Thread(target=self._handle_places_request, args=(payload,), daemon=True).start()

            elif topic == "blindstick/directions/request":
                # ESP32步行路线规划请求
                threading.Thread(target=self._handle_directions_request, args=(payload,), daemon=True).start()

        except Exception as e:
            print(f"[MQTT] Message handling error: {e}")

    def _handle_asr_upload(self, payload):
        """处理ESP32 MQTT分片上传的ASR音频（绕开热点对443的封锁）
        协议: {"type":"start","len":<PCM字节数>} → raw base64分片 → {"type":"end"} → 触发百度 → 发布 blindstick/asr/result
        """
        try:
            if payload.startswith("{"):
                data = json.loads(payload)
                t = data.get("type")
                if t == "start":
                    self.asr_upload_buf = bytearray()
                    self.asr_upload_active = True
                    print(f"[ASR-MQTT] 开始接收音频，PCM len={data.get('len')}")
                elif t == "end":
                    self.asr_upload_active = False
                    if len(self.asr_upload_buf) == 0:
                        print("[ASR-MQTT] 空音频，忽略")
                        return
                    try:
                        base64_data = bytes(self.asr_upload_buf).decode('ascii')
                        import base64 as _b64
                        pcm = _b64.b64decode(base64_data)
                        print(f"[ASR-MQTT] 收齐 base64={len(base64_data)} 解码PCM={len(pcm)} 字节")
                        err, text = _baidu_asr(base64_data, len(pcm))
                        if err == 0:
                            res = json.dumps({"err_no": 0, "text": text})
                        else:
                            res = json.dumps({"err_no": err, "msg": str(text)})
                    except Exception as e:
                        print(f"[ASR-MQTT] 解码/识别错误: {e}")
                        res = json.dumps({"err_no": -5, "msg": str(e)})
                    self.client.publish("blindstick/asr/result", res)
                    print(f"[ASR-MQTT] 已回传结果: {res[:60]}")
                    self.asr_upload_buf = bytearray()
            else:
                # 原始base64分片（MQTT有序到达，顺序追加）
                if self.asr_upload_active:
                    self.asr_upload_buf += payload.encode('ascii')
        except Exception as e:
            print(f"[ASR-MQTT] 处理错误: {e}")

    def _handle_places_request(self, payload):
        """ESP32地点搜索：调百度地图，压缩成固件解析需要的字段后回发
        响应只保留 status + results[].{name, location.lat/lng}，保证MQTT消息<8192
        """
        try:
            data = json.loads(payload)
            keyword = data.get("query", "")
            region = data.get("region", "黄石市")
            params = {
                "query": keyword,
                "region": region,
                "output": "json",
                "ak": "e9R2xrzLSwLzjMH5fdqHz4dLB0gXwIZW",
                "page_size": 5,
            }
            resp = requests.get("https://api.map.baidu.com/place/v2/search", params=params, timeout=15, verify=False)
            data = resp.json()
            slim = {"status": data.get("status", 2), "results": []}
            if slim["status"] == 0:
                slim["results"] = [
                    {"name": r.get("name", ""),
                     "location": {"lat": r["location"]["lat"], "lng": r["location"]["lng"]}}
                    for r in data.get("results", []) if r.get("location")
                ]
            print(f"[Places-MQTT] '{keyword}' status={slim['status']} results={len(slim['results'])}")
            self.client.publish("blindstick/places/result", json.dumps(slim))
        except Exception as e:
            print(f"[Places-MQTT] Error: {e}")
            try:
                self.client.publish("blindstick/places/result", json.dumps({"status": 2, "results": []}))
            except Exception:
                pass

    def _handle_directions_request(self, payload):
        """ESP32步行路线：调百度地图，压缩成固件解析需要的字段后回发
        响应只保留 result.routes[0].{distance, duration, steps[].instruction}
        """
        try:
            data = json.loads(payload)
            origin = data.get("origin", "")
            destination = data.get("destination", "")
            params = {
                "origin": origin,
                "destination": destination,
                "ak": "e9R2xrzLSwLzjMH5fdqHz4dLB0gXwIZW",
            }
            resp = requests.get("https://api.map.baidu.com/directionlite/v1/walking", params=params, timeout=15, verify=False)
            data = resp.json()
            slim = {"status": data.get("status", 2), "result": {"routes": []}}
            if slim["status"] == 0 and data.get("result") and data["result"].get("routes"):
                route = data["result"]["routes"][0]
                slim["result"]["routes"] = [{
                    "distance": route.get("distance", 0),
                    "duration": route.get("duration", 0),
                    "steps": [{"instruction": s.get("instruction", "")} for s in route.get("steps", [])],
                }]
            print(f"[Directions-MQTT] status={slim['status']} steps={len(slim['result']['routes'][0]['steps']) if slim['result']['routes'] else 0}")
            self.client.publish("blindstick/directions/result", json.dumps(slim))
        except Exception as e:
            print(f"[Directions-MQTT] Error: {e}")
            try:
                self.client.publish("blindstick/directions/result", json.dumps({"status": 2, "result": {"routes": []}}))
            except Exception:
                pass

    def handle_tts_request(self, text, priority=0):
        """处理TTS请求：调用百度TTS并推送URL到ESP32"""
        try:
            # 【优化】先检查缓存：相同文本的音频已存在则直接复用，跳过百度TTS合成
            file_hash = hashlib.md5(text.encode()).hexdigest()[:8]
            filename = f"tts_{file_hash}.wav"
            filepath = os.path.join(AUDIO_CACHE_DIR, filename)

            if os.path.exists(filepath):
                print(f"[TTS] 缓存命中，直接复用: {filename}")
                self._push_tts_audio(filename, text, priority)
                return

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
                "vol": 12,
                "per": 1,
                "aue": 6
            }

            # 【修复】超时从8s加到15s并失败重试一次：ESP32还在等这个结果，
            # 宁可慢一点也不要一次超时就让整条语音链路断掉
            resp = None
            for _attempt in range(2):
                try:
                    resp = requests.post(url, data=payload, timeout=15, verify=False)
                    break
                except Exception:
                    print(f"[TTS] 合成请求第{_attempt+1}次失败，重试..." if _attempt == 0 else "[TTS] 合成请求重试仍失败")
            if resp is None:
                print("[TTS] 合成请求失败（两次均异常）")
                return

            if 'audio' in resp.headers.get('Content-Type', ''):
                audio_data = resp.content
                print(f"[TTS] Synthesis success: {len(audio_data)} bytes")

                # 保存音频文件（固定文件名，相同文本复用）
                os.makedirs(AUDIO_CACHE_DIR, exist_ok=True)
                with open(filepath, 'wb') as f:
                    f.write(audio_data)
                print(f"[TTS] 音频已缓存: {filename}")

                self._push_tts_audio(filename, text, priority)
            else:
                print(f"[TTS] Synthesis failed: {resp.text[:200]}")

        except Exception as e:
            print(f"[TTS] Handle request error: {e}")
            import traceback
            traceback.print_exc()

    def _push_tts_audio(self, filename, text, priority=0):
        """【修复】把TTS音频经MQTT分片推给ESP32，绕开被热点/运营商挡住的Render 443下载。

        链路：本函数 → blindstick/tts/control(stream_start/end) + blindstick/tts/stream/N 分片
        → 固件重装成完整PCM → 独立播放任务播放。
        每分片须小于固件 mqtt.setBufferSize(8192)，用4000字节留余量。
        """
        filepath = os.path.join(AUDIO_CACHE_DIR, filename)
        if not os.path.exists(filepath):
            print(f"[TTS] 音频文件不存在: {filepath}")
            return
        try:
            with open(filepath, 'rb') as f:
                audio = f.read()
        except Exception as e:
            print(f"[TTS] 读取音频失败: {e}")
            return

        # 去掉WAV头（解析到data chunk，不硬编码44字节——部分编码器头更长），固件按裸PCM播放
        if len(audio) > 12 and audio[:4] == b'RIFF' and audio[8:12] == b'WAVE':
            offset = 12
            while offset + 8 <= len(audio):
                chunk_id = audio[offset:offset + 4]
                chunk_len = int.from_bytes(audio[offset + 4:offset + 8], 'little')
                if chunk_id == b'data':
                    offset += 8
                    break
                offset += 8 + chunk_len + (chunk_len & 1)  # 跳过对齐填充字节
            audio = audio[offset:]

        if len(audio) < 100:
            print(f"[TTS] 音频过短({len(audio)}字节)，跳过")
            return
        if len(audio) > 200 * 1024:
            print(f"[TTS] 音频过大({len(audio)}字节)，超过固件180KB上限，跳过")
            return

        if not (self.client and self.connected):
            print(f"[TTS] MQTT未连接，无法推送音频: {filename}")
            return

        chunk_size = 4000
        segments = (len(audio) + chunk_size - 1) // chunk_size
        session_id = int(time.time() * 1000) % 100000

        def pub(topic, payload):
            try:
                result = self.client.publish(topic, payload)
                return result.rc if hasattr(result, 'rc') else result[0]
            except Exception as e:
                print(f"[TTS] 发布异常 {topic}: {e}")
                return -1

        # 1) 会话开始
        rc = pub("blindstick/tts/control", json.dumps({
            "type": "stream_start",
            "priority": priority,
            "session_id": session_id,
            "segments": segments,
            # 【修复】带上TTS文本，固件据此填充 lastPlayedTtsText，
            # isTtsEcho 才能过滤喇叭回声（否则麦克风收回的TTS被当成语音命令）
            # 限40字：足够isTtsEcho做包含匹配，又不超固件StaticJsonDocument<512>池
            "text": (text or "")[:40]
        }))
        if rc != 0:
            print(f"[TTS] stream_start发布失败 rc={rc}，放弃本次播报")
            return

        # 2) 逐片推送PCM
        for i in range(segments):
            chunk = audio[i * chunk_size:(i + 1) * chunk_size]
            r = pub(f"blindstick/tts/stream/{i}", chunk)
            if r != 0:
                print(f"[TTS] 分片{i}发布失败 rc={r}，中断")
                return
            time.sleep(0.02)  # 让固件逐片消化，避免瞬时刷爆

        # 3) 会话结束
        pub("blindstick/tts/control", json.dumps({
            "type": "stream_end",
            "segments": segments,
            "session_id": session_id
        }))
        print(f"[TTS] MQTT音频推送完成: {len(audio)}字节/{segments}片 session={session_id}")

    def connect(self):
        if not MQTT_AVAILABLE:
            print("[MQTT] paho-mqtt not available")
            return False

        if self.connected:
            return True

        try:
            # 【修复】如果已有client且正在运行，先停止旧连接
            if self.client is not None:
                try:
                    self.client.disconnect()
                except Exception:
                    pass
                try:
                    self.client.loop_stop()
                except Exception:
                    pass

            # 使用新版 API (paho-mqtt >= 2.0) 或旧版 API
            if MQTT_V2:
                self.client = mqtt.Client(
                    callback_api_version=CallbackAPIVersion.VERSION2,
                    client_id=self.client_id
                )
            else:
                self.client = mqtt.Client(client_id=self.client_id)

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
                    # 订阅TTS请求 + ASR上传 + 地点/路线请求（全部走MQTT绕开443）
                    try:
                        client.subscribe("blindstick/tts/request")
                        client.subscribe("blindstick/asr/upload")
                        client.subscribe("blindstick/places/request")
                        client.subscribe("blindstick/directions/request")
                        print("[MQTT] Subscribed: tts/request, asr/upload, places/request, directions/request")
                    except Exception as e:
                        print(f"[MQTT] 订阅失败: {e}")
                else:
                    print(f"[MQTT] Connection failed, code: {reason_code}")

            def on_disconnect(client, userdata, disconnect_flags=None, rc=None, properties=None):
                """处理断开连接，兼容新旧版本"""
                self.connected = False
                # 处理不同版本的参数
                if rc is not None:
                    rc_val = rc.value if hasattr(rc, 'value') else rc
                    print(f"[MQTT] Disconnected, code: {rc_val}")
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
                print(f"[MQTT] Will retry ({self.connect_retry_count})")
            return False

    def reconnect_if_needed(self):
        """检查连接状态，如需要则重连（无限重试）"""
        if not self.connected:
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
    """提供TTS音频文件下载（ESP32通过URL下载）
    【修复】send_from_directory 走流式/chunked传输（无Content-Length头），
    ESP32的 http.getSize() 返回-1直接拒绝下载 → 改为整读进内存显式带Content-Length
    """
    if '..' in filename or filename.startswith('/'):
        return "Invalid filename", 400
    filepath = os.path.join(AUDIO_CACHE_DIR, filename)
    if not os.path.exists(filepath):
        return "Audio file not found", 404
    with open(filepath, 'rb') as f:
        data = f.read()
    resp = Response(data, mimetype='audio/wav')
    resp.headers['Content-Length'] = str(len(data))
    return resp

@app.route('/health', methods=['GET'])
def health_check():
    """健康检查接口"""
    return jsonify({
        "status": "ok",
        "mqtt_connected": mqtt_sender.connected,
        "mode": "mqtt_only"
    })

# ==================== ASR Proxy（ESP32语音识别代理）====================
@app.route('/api/asr', methods=['POST'])
def asr_proxy():
    """ESP32识别代理：热点拦ESP32直连百度，改由后端转发
    请求: {"speech": "<base64 PCM 16k单声道>", "len": <音频字节数>}
    响应: {"err_no": 0, "text": "识别文字"} 或 {"err_no": x, "msg": "..."}
    """
    try:
        data = request.get_json(force=True, silent=True)
        if not data:
            return jsonify({"err_no": -1, "msg": "invalid json"}), 400
        speech = data.get("speech", "")
        length = data.get("len", 0)
        if not speech or not length:
            return jsonify({"err_no": -2, "msg": "missing speech/len"}), 400

        err, text = _baidu_asr(speech, length)
        if err == 0:
            return jsonify({"err_no": 0, "text": text})
        return jsonify({"err_no": err, "msg": str(text)}), (500 if err == -3 or err == -5 else 200)
    except Exception as e:
        print(f"[ASR] Error: {e}")
        import traceback
        traceback.print_exc()
        return jsonify({"err_no": -5, "msg": str(e)}), 500

# ==================== Places Proxy（ESP32地点搜索代理）====================
@app.route('/api/places', methods=['GET'])
def places_proxy():
    """ESP32地点搜索代理：热点拦ESP32直连百度地图API，改由后端转发
    请求: /api/places?query=湖北师范大学&region=黄石市
    响应: 百度地图Place API原始JSON（status + results[].name/location）
    """
    keyword = request.args.get('query', '')
    region = request.args.get('region', '黄石市')
    if not keyword:
        return jsonify({"status": 1, "message": "missing query"}), 400
    try:
        params = {
            "query": keyword,
            "region": region,
            "output": "json",
            "ak": "e9R2xrzLSwLzjMH5fdqHz4dLB0gXwIZW",
            "page_size": 5,
        }
        resp = requests.get("https://api.map.baidu.com/place/v2/search", params=params, timeout=15, verify=False)
        data = resp.json()
        print(f"[Places] '{keyword}' status={data.get('status')} results={len(data.get('results', []))}")
        return jsonify(data)
    except Exception as e:
        print(f"[Places] Error: {e}")
        import traceback
        traceback.print_exc()
        return jsonify({"status": 2, "message": str(e)}), 500

# ==================== Directions Proxy（ESP32步行路线代理）====================
@app.route('/api/directions', methods=['GET'])
def directions_proxy():
    """ESP32步行路线代理：热点拦ESP32直连百度地图API，改由后端转发
    请求: /api/directions?origin=lat,lng&destination=lat,lng
    响应: 百度directionlite步行路线原始JSON
    """
    origin = request.args.get('origin', '')
    destination = request.args.get('destination', '')
    if not origin or not destination:
        return jsonify({"status": 1, "message": "missing origin/destination"}), 400
    try:
        params = {
            "origin": origin,
            "destination": destination,
            "ak": "e9R2xrzLSwLzjMH5fdqHz4dLB0gXwIZW",
        }
        resp = requests.get("https://api.map.baidu.com/directionlite/v1/walking", params=params, timeout=15, verify=False)
        data = resp.json()
        if data.get("status") == 0 and data.get("result") and data["result"].get("routes"):
            print(f"[Directions] OK, routes={len(data['result']['routes'])}")
        else:
            print(f"[Directions] status={data.get('status')} msg={data.get('message')}")
        return jsonify(data)
    except Exception as e:
        print(f"[Directions] Error: {e}")
        import traceback
        traceback.print_exc()
        return jsonify({"status": 2, "message": str(e)}), 500

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

    # 启动 MQTT 连接（持续运行，断开自动重连）
    print("[Startup] Starting MQTT connection...")
    def mqtt_keepalive_loop():
        """持续运行：连接成功后保持监控，断开后自动重连（无限重试）"""
        while True:
            if mqtt_sender.connected:
                # 已连接，每10秒检查一次
                time.sleep(10)
            else:
                # 尝试连接（connect内部会处理旧client）
                success = mqtt_sender.connect()
                # 即使connect()返回True，也要确认connected标志（可能有立即断开的情况）
                if not success or not mqtt_sender.connected:
                    print("[MQTT] 连接未建立，3秒后重试...")
                    time.sleep(3)
                else:
                    print("[MQTT] MQTT连接已建立，开始监控")
                    time.sleep(10)

    # 在后台线程启动 MQTT 连接守护循环
    import threading
    mqtt_thread = threading.Thread(target=mqtt_keepalive_loop, daemon=True)
    mqtt_thread.start()

    # 【新增】防止Render免费实例休眠：定时自我唤醒
    def keepalive_self_ping():
        """每14分钟访问一次自身/health，防止Render实例休眠"""
        import urllib.request
        while True:
            time.sleep(14 * 60)  # 14分钟一次（免费层15分钟休眠）
            try:
                port = int(os.environ.get('PORT', 8090))
                req = urllib.request.Request(
                    f"http://localhost:{port}/health",
                    headers={'User-Agent': 'Mozilla/5.0'}
                )
                with urllib.request.urlopen(req, timeout=10) as resp:
                    print(f"[KeepAlive] 自我唤醒: {resp.status}")
            except Exception as e:
                print(f"[KeepAlive] 自我唤醒失败(可忽略): {e}")

    # 启动自我唤醒线程（仅在非Render本地运行时也保持）
    keepalive_thread = threading.Thread(target=keepalive_self_ping, daemon=True)
    keepalive_thread.start()

    # 启动 Flask 服务
    app.run(host='0.0.0.0', port=port, debug=False)
