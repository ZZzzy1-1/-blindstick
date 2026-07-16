#!/usr/bin/env python3
"""
百度API代理服务器 - 解决浏览器CORS问题
运行: python proxy_server.py
"""

from flask import Flask, request, jsonify, Response, send_from_directory
from flask_cors import CORS
import requests
import json
import os
import time
import hashlib

app = Flask(__name__)
CORS(app, resources={r"/*": {"origins": "*"}})

# ==================== 前端静态文件托管 ====================
BASE_DIR = os.path.dirname(os.path.abspath(__file__))
FRONTEND_DIR = os.path.dirname(BASE_DIR)   # 项目根目录（index.html 所在处）

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

# 百度API配置
BAIDU_API_KEY = "Xbxnhkwb2sxtB6HbH5BUTlUG"
BAIDU_SECRET_KEY = "Tw485P2BFGpPu8WeOVP6hy4S1BHqG4ON"

# 缓存token
cached_token = {
    "access_token": None,
    "expires_at": 0
}

# 缓存TTS结果，防止频繁请求相同文本
tts_cache = {}
TTS_CACHE_MAX_SIZE = 100  # 最大缓存条目
TTS_CACHE_TTL = 10  # 缓存生存时间（秒）

def get_baidu_token():
    """获取百度Access Token（带缓存）"""
    # 检查缓存
    if cached_token["access_token"] and time.time() < cached_token["expires_at"] - 300:
        print("[Token] 使用缓存")
        return cached_token["access_token"]

    # 请求新token
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

def _clean_tts_cache():
    """清理过期的 TTS 缓存条目"""
    now = time.time()
    to_del = []
    for k, v in tts_cache.items():
        if now - v['timestamp'] > TTS_CACHE_TTL:
            to_del.append(k)
    for k in to_del:
        del tts_cache[k]
    # 如果仍然超限，删除最旧的
    while len(tts_cache) > TTS_CACHE_MAX_SIZE:
        # 删除最早的一项
        oldest_key = next(iter(tts_cache))
        del tts_cache[oldest_key]

@app.route('/api/tts', methods=['POST'])
def text_to_speech():
    """文本转语音接口"""
    try:
        # 兼容 Werkzeug 2.3.x 的 get_json 问题
        if request.is_json:
            data = request.get_json(force=True, silent=True)
        else:
            data = {}
        if data is None:
            raw = request.get_data(as_text=True)
            try:
                data = json.loads(raw)
            except Exception:
                data = {}

        text = (data or {}).get('text', '')

        if not text:
            return jsonify({"error": "缺少text参数"}), 400

        print(f"[TTS] 请求: {text[:30]}...")

        # 检查缓存
        _clean_tts_cache()
        cache_key = hashlib.md5(text.encode('utf-8')).hexdigest()
        cached = tts_cache.get(cache_key)
        if cached and (time.time() - cached['timestamp'] < TTS_CACHE_TTL):
            print(f"[TTS] 使用缓存结果，大小: {len(cached['data'])} 字节")
            return Response(cached['data'], mimetype='audio/wav')

        # 获取token
        token = get_baidu_token()
        if not token:
            return jsonify({"error": "无法获取Token"}), 500

        # 调用百度TTS（使用基础音库，避免权限问题）
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
            "per": 1,  # 改为度小宇（也可能是per=0权限问题）
            "aue": 6   # wav格式
        }

        resp = requests.post(url, data=payload, timeout=15, verify=False)

        content_type = resp.headers.get('Content-Type', '')
        print(f"[TTS] 百度响应 Content-Type: {content_type}")
        print(f"[TTS] 百度响应状态: {resp.status_code}")

        if 'audio' in content_type:
            print(f"[TTS] 成功，音频大小: {len(resp.content)} 字节")
            # 缓存结果
            tts_cache[cache_key] = {
                'data': resp.content,
                'timestamp': time.time()
            }
            return Response(resp.content, mimetype='audio/wav')
        else:
            print(f"[TTS] 错误: {resp.text[:500]}")
            return jsonify({"error": "TTS失败", "detail": resp.text}), 500

    except Exception as e:
        print(f"[TTS] 异常: {e}")
        return jsonify({"error": str(e)}), 500

@app.route('/api/asr', methods=['POST'])
def speech_to_text():
    """语音识别接口"""
    try:
        # 获取PCM音频数据
        pcm_data = request.data

        if len(pcm_data) < 1000:
            return jsonify({"error": "音频数据太短"}), 400

        print(f"[ASR] 收到音频: {len(pcm_data)} 字节")

        # 获取token
        token = get_baidu_token()
        if not token:
            return jsonify({"error": "无法获取Token"}), 500

        # 调用百度ASR
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
    return jsonify({"status": "ok"})

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
    """获取用户配置"""
    try:
        cfg = _load_config()
        return jsonify({"status": "ok", "home_city": cfg.get("home_city", "黄石市")})
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/config', methods=['POST'])
def set_config():
    """保存用户配置"""
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

# ==================== 导航停止接口 ====================
@app.route('/api/navigation/stop', methods=['POST'])
def stop_navigation():
    """停止导航"""
    print("[导航] 收到停止导航请求")
    return jsonify({"status": "ok", "message": "导航已停止"})

# ==================== 导航路线接口（ESP32调用）====================
# 2025-07-16: 新增此接口供ESP32获取导航路线
@app.route('/api/nav_steps', methods=['GET', 'POST'])
def get_nav_steps():
    """获取导航路线步骤（ESP32用）"""
    print("[导航] 收到路线请求")
    # 返回示例导航步骤，实际应从请求参数获取目的地
    return jsonify({
        "status": "ok",
        "steps": [
            "前方直行100米",
            "左转进入主干道",
            "继续直行200米",
            "右转到达目的地"
        ]
    })

# ==================== TTS 分段音频接口（支持大音频分段上传）====================
@app.route('/api/tts/segments', methods=['POST'])
def tts_segments():
    """接收分段TTS音频并合并返回"""
    try:
        data = request.get_json()
        if not data or 'segments' not in data:
            return jsonify({"error": "缺少segments参数"}), 400

        segments_info = data['segments']
        merged = b''
        for seg in segments_info:
            idx = seg.get('index')
            # 客户端应该已分段合并好，这里直接接收
            # 实际上分段通过多个 /api/tts/segment/{idx} 接口接收
            pass

        return jsonify({"status": "ok"})
    except Exception as e:
        return jsonify({"error": str(e)}), 500

@app.route('/api/tts/segment/<int:idx>', methods=['POST'])
def tts_segment(idx):
    """接收单个TTS音频分段"""
    try:
        seg_data = request.get_data()
        # 将分段存储在临时目录
        seg_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'tts_segments')
        os.makedirs(seg_dir, exist_ok=True)
        seg_path = os.path.join(seg_dir, f'seg_{idx}.bin')
        with open(seg_path, 'wb') as f:
            f.write(seg_data)
        print(f"[TTS] 收到分段 {idx}: {len(seg_data)} 字节")
        return jsonify({"status": "ok", "index": idx, "size": len(seg_data)})
    except Exception as e:
        return jsonify({"error": str(e)}), 500

if __name__ == '__main__':
    import urllib3
    urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

    # Render 通过 PORT 环境变量指定端口，本地默认 8090
    port = int(os.environ.get('PORT', 8090))

    print("=" * 50)
    print("百度API代理服务器 + 导盲杖大屏")
    print("=" * 50)
    print("API接口:")
    print("  POST /api/tts            - 文本转语音")
    print("  POST /api/tts/segment/:i - TTS音频分段上传")
    print("  POST /api/tts/segments   - TTS分段元数据")
    print("  POST /api/asr            - 语音识别")
    print("  GET  /api/config         - 获取用户配置")
    print("  POST /api/config         - 保存用户配置")
    print("  POST /api/navigation/stop- 停止导航")
    print("  GET  /health             - 健康检查")
    print("  GET  /                   - 前端大屏首页")
    print("=" * 50)
    print(f"运行在: http://0.0.0.0:{port}")
    print(f"大屏地址: http://localhost:{port}")
    print("=" * 50)

    app.run(host='0.0.0.0', port=port, debug=False)