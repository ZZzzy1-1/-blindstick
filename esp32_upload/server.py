"""
========================================
智能导盲杖 - 主控服务器 (整合版)
========================================
功能：
  1. 接收 ESP32 雷达/GPS数据
  2. 接收 K230 视觉检测结果
  3. 语音AI对话 (百度ASR + 通义千问 + 百度TTS)
  4. 障碍物语音告警播报
  5. 高德地图导航API
  6. WebSocket实时推送

运行方式:
  python server.py

端口: 8080
"""

import os
import sys
import json
import time
import base64
import asyncio
import threading
import requests
import concurrent.futures
from datetime import datetime

from flask import Flask, request, jsonify, send_from_directory, Response
from flask_sock import Sock

# 禁用SSL警告
import urllib3
urllib3.disable_warnings(urllib3.exceptions.InsecureRequestWarning)

app = Flask(__name__)
sock = Sock(app)

# ==================== CORS 支持 ====================
@app.after_request
def after_request(response):
    """添加 CORS 响应头，允许跨域请求"""
    response.headers.add('Access-Control-Allow-Origin', '*')
    response.headers.add('Access-Control-Allow-Headers', 'Content-Type,Authorization')
    response.headers.add('Access-Control-Allow-Methods', 'GET,PUT,POST,DELETE,OPTIONS')
    return response

# ==================== 配置区 ====================
PORT = 8080

# 🌟 核心适配：当前 BASE_DIR 是 visualization/server/
BASE_DIR = os.path.dirname(os.path.abspath(__file__))

# 🌟 核心适配：向外跳一级到 visualization/，作为网页资源的根目录
STATIC_FOLDER = os.path.dirname(BASE_DIR)

# 保存所有连接的网页端客户端
web_clients = set()

# ==================== API密钥配置 ====================
# 百度AI (ASR + TTS)
BAIDU_APPID = "123607377"
BAIDU_API_KEY = "Xbxnhkwb2sxtB6HbH5BUTlUG"
BAIDU_SECRET_KEY = "Tw485P2BFGpPu8WeOVP6hy4S1BHqG4ON"

# 通义千问 (LLM对话)
DASHSCOPE_API_KEY = "sk-57df3af8a02e485ca61469fa10f68c7e"

# 百度地图
BAIDU_MAP_AK = "e9R2xrzLSwLzjMH5fdqHz4dLB0gXwIZW"

# ==================== 语音配置 ====================
# 导航触发词（必须包含这些明确的导航意图词才会解析为目的地）
# 注意：触发词按长度降序排列，优先匹配更具体的短语
NAV_TRIGGERS = ["带我去", "我要去", "我想去", "导航到", "要去", "去"]

# 需要过滤的无意义词汇（ASR识别错误产生的）
# 注意：长词放在前面，避免部分匹配问题
FILTER_WORDS = ["但是", "那个", "这个", "但", "我", "啊", "嗯", "哦", "呢", "吧", "是", "去",
                "他", "她", "的", "自己", "都", "在", "柏", "脖子", "听到", "听到说", "都是",
                # 🌟 避障语音回声的过滤词
                "厘米", "障碍", "避让", "危险", "立即", "建议", "左侧", "右侧", "前方",
                # 🌟 ASR误识别的过滤词
                "为什么", "什么", "这句话", "这句话识", "识别", "差不多", "认识", "对因",
                "因为", "所以", "然后", "但是", "可是", "不过", "就是", "那个", "那个啥",
                "那个什么", "那个谁", "那个东西", "那个地方", "里面", "外面", "这里", "那里"]

# 有效的地点名称后缀（用于验证目的地是否像真实地点）
VALID_LOCATION_SUFFIXES = [
    "大学", "学院", "学校", "医院", "公园", "广场", "商场", "超市", "市场",
    "车站", "机场", "码头", "酒店", "宾馆", "饭店", "餐厅", "银行", "公司",
    "小区", "花园", "大厦", "大楼", "中心", "城", "馆", "局", "所", "处",
    "路", "街", "大道", "巷", "弄", "号", "栋", "单元", "楼", "层",
    "省", "市", "区", "县", "镇", "乡", "村",
    # 省份/自治区简称
    "京", "津", "沪", "渝", "冀", "豫", "云", "辽", "黑", "湘",
    "皖", "鲁", "新", "苏", "浙", "赣", "鄂", "桂", "甘", "晋",
    "蒙", "陕", "吉", "闽", "贵", "粤", "青", "藏", "川", "宁", "琼", "港", "澳", "台"
]

# 固定语音告警文本
VOICE_ALERTS = {
    "front_obstacle": "前方有障碍物，请小心慢行",
    "left_obstacle": "左侧有障碍物，建议靠右避让",
    "right_obstacle": "右侧有障碍物，建议靠左避让",
    "front_critical": "前方危险！请立即停下",
    "nav_start": "导航已开始，请跟随牵引前行",
    "nav_arrive": "已到达目的地附近",
    "person_front": "前方有行人，请注意避让",
    "car_front": "前方有车辆，请等待通过",
    "stairs_front": "前方有台阶，请小心",
    "pit_front": "前方有坑洼，请绕行",
    "pole_front": "前方有立柱，请避让",
}

# ==================== 用户配置 ====================
class UserConfig:
    """用户配置管理器（常住地设置等）"""
    CONFIG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "user_config.json")

    def __init__(self):
        self.home_city = "黄石市"  # 默认常住地
        self.load()

    def load(self):
        """从文件加载配置"""
        try:
            if os.path.exists(self.CONFIG_FILE):
                with open(self.CONFIG_FILE, 'r', encoding='utf-8') as f:
                    data = json.load(f)
                    self.home_city = data.get("home_city", "黄石市")
                    print(f"[配置] 已加载用户配置，常住地: {self.home_city}")
        except Exception as e:
            print(f"[配置] 加载配置失败: {e}，使用默认值")

    def save(self):
        """保存配置到文件"""
        try:
            with open(self.CONFIG_FILE, 'w', encoding='utf-8') as f:
                json.dump({"home_city": self.home_city}, f, ensure_ascii=False, indent=2)
            print(f"[配置] 已保存配置，常住地: {self.home_city}")
            return True
        except Exception as e:
            print(f"[配置] 保存配置失败: {e}")
            return False

    def set_home_city(self, city):
        """设置常住地城市"""
        if city and city.strip():
            self.home_city = city.strip()
            return self.save()
        return False

    def get_home_city(self):
        """获取常住地城市"""
        return self.home_city

# 全局用户配置实例
user_config = UserConfig()

# ==================== 全局状态 ====================
class SystemState:
    """系统状态管理器"""
    def __init__(self):
        # 设备状态
        self.esp32_online = False
        self.k230_online = False
        self.gps_valid = False

        # 最新位置
        self.current_lat = 0.0
        self.current_lng = 0.0
        self.current_speed = 0.0
        self.gps_satellites = 0

        # 雷达数据
        self.radar_front = 400.0
        self.radar_left = 400.0
        self.radar_right = 400.0
        self.is_blocked = False

        # 最新障碍物检测
        self.latest_detections = []
        self.last_detection_time = 0

        # 导航状态
        self.nav_active = False
        self.nav_destination = ""
        self.nav_steps = []
        self.nav_step_points = []  # 每步的坐标点
        self.current_step = 0
        self.nav_step_text = "等待导航开始"
        self.step_threshold = 10  # 到达步骤终点的阈值（米）
        self.nav_interrupted = False  # 导航是否被障碍物打断
        self.interrupted_nav_text = ""  # 被打断时的导航文本

        # 语音状态
        self.last_voice_alert = ""
        self.last_alert_time = 0
        self.alert_cooldown = 20  # 告警冷却时间（秒），20秒播报一次
        self.last_nav_speak_time = 0  # 上次导航语音播报时间
        self.nav_speak_duration = 5  # 导航语音预估播放时长（秒）

        # 事件记录（持久化）
        self.event_logs = []  # 最多保存100条

        # 语音对话记录（持久化）
        self.chat_logs = []  # 最多保存50条

        # 导航记录（持久化）
        self.nav_history = []  # 最多保存20条

        # 统计数据（持久化）
        self.stats = {
            "total_mileage": 0.0,  # 总里程（米）
            "nav_count": 0,        # 导航次数
            "obstacle_count": 0,   # 障碍物提醒次数
            "detour_count": 0      # 路线调整次数
        }

    def to_dict(self):
        return {
            "esp32_online": self.esp32_online,
            "k230_online": self.k230_online,
            "gps_valid": self.gps_valid,
            "lat": self.current_lat,
            "lng": self.current_lng,
            "speed": self.current_speed,
            "satellites": self.gps_satellites,
            "radar": {
                "front": self.radar_front,
                "left": self.radar_left,
                "right": self.radar_right
            },
            "blocked": self.is_blocked,
            "detections": self.latest_detections,
            "nav_active": self.nav_active,
            "nav_destination": self.nav_destination,
            "nav_step": self.nav_step_text,
            "nav_steps": self.nav_steps,
            "current_step": self.current_step,
            "nav_step_points": self.nav_step_points,
        }

system_state = SystemState()

# ==================== 障碍物告警TTS异步投递缓存 ====================
# 后台线程合成 TTS 音频后缓存到这里，下次 ESP32 轮询时立即返回
_pending_audio = {"data": None, "lock": threading.Lock()}

def _synthesize_and_cache_alert(alert_text, priority=0):
    """后台合成障碍物告警 TTS，缓存到 _pending_audio"""
    print(f"[TTS缓存] 开始异步合成: '{alert_text[:20]}...' 优先级={priority}")
    try:
        wav = baidu_service.tts(alert_text)
        if wav and len(wav) > 0:
            with _pending_audio["lock"]:
                _pending_audio["data"] = (alert_text, wav, priority)
            print(f"[TTS缓存] ✅ 合成完成，缓存音频 {len(wav)} 字节，等待ESP32轮询取走")
        else:
            print(f"[TTS缓存] ⚠️ 合成返回为空")
    except Exception as e:
        print(f"[TTS缓存] ❌ 合成异常: {e}")

# ==================== 事件记录管理 ====================
def add_event_log(event_type, message):
    """添加事件记录（最多保存100条）"""
    from datetime import datetime
    event = {
        "type": event_type,
        "message": message,
        "time": datetime.now().strftime("%H:%M:%S"),
        "timestamp": time.time()
    }
    system_state.event_logs.insert(0, event)  # 新事件放前面
    # 限制数量
    if len(system_state.event_logs) > 100:
        system_state.event_logs = system_state.event_logs[:100]
    return event

def get_event_logs(limit=50):
    """获取事件记录"""
    return system_state.event_logs[:limit]

# ==================== 语音对话记录管理 ====================
def add_chat_log(user_text, system_text):
    """添加语音对话记录（最多保存50条）"""
    from datetime import datetime
    chat = {
        "user": user_text,
        "system": system_text,
        "time": datetime.now().strftime("%H:%M:%S"),
        "timestamp": time.time()
    }
    system_state.chat_logs.insert(0, chat)
    # 限制数量
    if len(system_state.chat_logs) > 50:
        system_state.chat_logs = system_state.chat_logs[:50]
    return chat

def get_chat_logs(limit=50):
    """获取语音对话记录"""
    return system_state.chat_logs[:limit]

# ==================== 导航记录管理 ====================
def add_nav_log(destination, steps_count):
    """添加导航记录（最多保存20条）"""
    from datetime import datetime
    nav = {
        "destination": destination,
        "steps_count": steps_count,
        "time": datetime.now().strftime("%H:%M:%S"),
        "timestamp": time.time()
    }
    system_state.nav_history.insert(0, nav)
    # 限制数量
    if len(system_state.nav_history) > 20:
        system_state.nav_history = system_state.nav_history[:20]
    return nav

def get_nav_logs(limit=20):
    """获取导航记录"""
    return system_state.nav_history[:limit]


# ==================== 统计数据管理 ====================
def update_stats(nav_count=None, obstacle_count=None, detour_count=None, mileage=None):
    """更新统计数据"""
    if nav_count is not None:
        system_state.stats["nav_count"] = nav_count
    if obstacle_count is not None:
        system_state.stats["obstacle_count"] = obstacle_count
    if detour_count is not None:
        system_state.stats["detour_count"] = detour_count
    if mileage is not None:
        system_state.stats["total_mileage"] = mileage

def get_stats():
    """获取统计数据"""
    return system_state.stats

# ==================== 百度AI服务 ====================
class BaiduAIService:
    """百度AI服务封装 (ASR + TTS)"""
    def __init__(self):
        self._token = None
        self._token_expire = 0
        self._lock = threading.Lock()

    def get_token(self):
        """获取百度Access Token"""
        with self._lock:
            if self._token and time.time() < self._token_expire - 300:
                return self._token

        url = f"https://aip.baidubce.com/oauth/2.0/token?grant_type=client_credentials&client_id={BAIDU_API_KEY}&client_secret={BAIDU_SECRET_KEY}"
        try:
            r = requests.post(url, timeout=10, verify=False).json()
            with self._lock:
                self._token = r["access_token"]
                self._token_expire = time.time() + r.get("expires_in", 2592000)
            print(f"[百度AI] Token刷新成功")
            return self._token
        except Exception as e:
            print(f"[百度AI] Token获取失败: {e}")
            return None

    def asr(self, pcm_bytes):
        """语音识别"""
        token = self.get_token()
        if not token:
            return None

        url = "https://vop.baidu.com/server_api"
        payload = {
            "format": "pcm",
            "rate": 16000,
            "channel": 1,
            "cuid": "blind_stick_001",
            "token": token,
            "dev_pid": 1537,
            "speech": base64.b64encode(pcm_bytes).decode('utf-8'),
            "len": len(pcm_bytes)
        }

        try:
            resp = requests.post(url, json=payload, timeout=60, verify=False)
            r = resp.json()
            if r.get("err_no") == 0:
                return r.get("result", [""])[0]
            else:
                print(f"[ASR] 百度返回错误: {r.get('err_msg')}")
                return None
        except Exception as e:
            print(f"[ASR] 请求失败: {e}")
            return None

    def tts(self, text, per=5118):
        """语音合成"""
        print(f"[TTS] 开始合成: '{text[:30]}...'")
        token = self.get_token()
        if not token:
            print("[TTS] ❌ 无法获取token")
            return None

        url = "https://tsn.baidu.com/text2audio"
        payload = {
            "tex": text,
            "tok": token,
            "cuid": "blind_stick_001",
            "ctp": 1,
            "lan": "zh",
            "spd": 4,
            "pit": 5,
            "vol": 9,  # 音量 0-15，默认15最大，这里调小为9
            "per": per,
            "aue": 6,
        }

        try:
            r = requests.post(url, data=payload, timeout=15, verify=False)
            content_type = r.headers.get("Content-Type", "")
            print(f"[TTS] 响应Content-Type: {content_type}")
            if "audio" in content_type:
                print(f"[TTS] ✅ 合成成功，音频大小: {len(r.content)} 字节")
                return r.content
            else:
                print(f"[TTS] ❌ 返回的不是音频: {r.text[:100]}")
                return None
        except Exception as e:
            print(f"[TTS] ❌ 请求失败: {e}")
            return None

baidu_service = BaiduAIService()

# ==================== TTS 队列管理 ====================
class TTSQueue:
    """TTS语音队列管理器 - 避免导航和避障播报冲突"""
    def __init__(self):
        self.queue = []
        self.is_playing = False
        self._lock = threading.Lock()
        self._thread = None

    def add(self, text, priority=1):
        """
        添加语音到队列
        priority: 1=普通(对话回复), 2=高优先级(避障告警), 3=最高优先级(用户导航指令)
        """
        with self._lock:
            # 如果是最高优先级（导航指令），清空低优先级任务，立即播放
            if priority >= 3:
                # 保留同优先级或更高优先级的任务，移除低优先级避障告警
                original_len = len(self.queue)
                self.queue = [item for item in self.queue if item["priority"] >= 3]
                removed = original_len - len(self.queue)
                if removed > 0:
                    print(f"[TTS队列] 导航指令打断，移除{removed}个低优先级任务")

            self.queue.append({"text": text, "priority": priority, "time": time.time()})
            # 按优先级排序（高优先级在前）
            self.queue.sort(key=lambda x: (-x["priority"], x["time"]))
            print(f"[TTS队列] 添加任务: '{text[:20]}...', 优先级={priority}, 队列长度={len(self.queue)}")

    def play_next(self):
        """播放队列中的下一个语音"""
        with self._lock:
            if not self.queue:
                self.is_playing = False
                return False

            item = self.queue.pop(0)
            self.is_playing = True

        text = item["text"]
        print(f"[TTS队列] 播放: '{text[:30]}...'")

        try:
            wav = baidu_service.tts(text)
            if wav:
                # 这里我们只广播给前端，实际播放由ESP32处理
                broadcast_to_clients({
                    "type": "tts_queue",
                    "text": text,
                    "priority": item["priority"],
                    "audio_data": base64.b64encode(wav).decode('utf-8') if wav else None
                })
                # 模拟播放时间（根据文本长度估算）
                play_time = len(text) * 0.15  # 每个字约0.15秒
                time.sleep(play_time)
        except Exception as e:
            print(f"[TTS队列] 播放失败: {e}")

        with self._lock:
            self.is_playing = False
        return True

    def start(self):
        """启动队列处理线程"""
        if self._thread is None or not self._thread.is_alive():
            self._thread = threading.Thread(target=self._process_queue, daemon=True)
            self._thread.start()

    def _process_queue(self):
        """后台处理队列"""
        while True:
            if self.queue and not self.is_playing:
                self.play_next()
            time.sleep(0.1)

tts_queue = TTSQueue()
tts_queue.start()

# ==================== 通义千问LLM ====================
def qwen_chat(user_text):
    """通义千问对话"""
    url = "https://dashscope.aliyuncs.com/compatible-mode/v1/chat/completions"
    headers = {
        "Authorization": f"Bearer {DASHSCOPE_API_KEY}",
        "Content-Type": "application/json"
    }
    body = {
        "model": "qwen-turbo",
        "messages": [
            {
                "role": "system",
                "content": "你是智能导盲助手，名字叫'小杖'。请用非常简洁的中文回复（不超过30个字），给出导航指引或安全提醒。语气亲切，像家人一样。"
            },
            {"role": "user", "content": user_text}
        ]
    }

    try:
        r = requests.post(url, headers=headers, json=body, timeout=15, verify=False).json()
        return r["choices"][0]["message"]["content"]
    except Exception as e:
        print(f"[LLM] 请求失败: {e}")
        return "抱歉，我没听清，请再说一遍"

# ==================== 百度地图服务 ====================
def is_valid_destination(dest_name):
    """
    验证目的地是否看起来像有效的地名
    返回: (是否有效, 错误信息, 距离提示类型)
    距离提示类型: None(正常), "too_far"(太远), "too_short"(太短)
    """
    # 1. 检查长度（至少1个字符，距离判断由后续路线规划处理）
    if len(dest_name) < 1:
        return False, "目的地为空", "too_short"
    if len(dest_name) > 20:
        return False, "目的地太长", None

    # 2. 检查是否包含标点符号（有效地名通常不含标点）
    import re
    if re.search(r'[，。？！.,?!]+', dest_name):
        return False, "目的地包含标点符号"

    # 3. 检查是否包含数字（有效地名通常不含数字，除了"1号"、"2楼"等）
    if re.search(r'\d', dest_name):
        # 允许"1号楼"、"2层"等，但不允许"123"或"27厘米"
        if not re.search(r'\d+[号楼层室]', dest_name):
            return False, "目的地包含非法数字"

    # 4. 检查是否包含常见的地点后缀
    valid_suffixes = ['路', '街', '道', '巷', '号', '楼', '层', '室', '单元', '栋',
                      '大学', '学院', '学校', '医院', '公园', '广场', '大厦', '中心',
                      '店', '馆', '所', '局', '站', '厂', '村', '镇', '乡', '区',
                      '城', '市', '县', '州', '府', '门', '桥', '山', '河', '湖', '海',
                      '省', '京', '津', '沪', '渝', '疆', '藏', '蒙', '宁', '桂']

    has_valid_suffix = any(suffix in dest_name for suffix in valid_suffixes)

    # 4.1 短地名（2-3个字）放宽检查，让省份/城市名能进入路线规划
    if len(dest_name) <= 3 and len(re.findall(r'[一-鿿]', dest_name)) >= 2:
        has_valid_suffix = True  # 短地名（如"新疆"、"北京"）允许通过，由路线规划判断

    # 5. 检查是否只包含中文字符（允许少量英文字母，如"KFC"、"ATM"）
    chinese_chars = re.findall(r'[一-鿿]', dest_name)
    if len(chinese_chars) < 2:
        return False, "目的地需要包含至少2个汉字"

    # 6. 如果既没有有效后缀，又包含异常词汇，则可能是误识别
    if not has_valid_suffix:
        invalid_words = ['厘米', '毫米', '米', '分米', '障碍', '避让', '危险', '什么',
                        '为什么', '怎么', '哪里', '东西', '地方', '那个', '这个']
        if any(word in dest_name for word in invalid_words):
            return False, "目的地包含非地点词汇"

    return True, None


def get_baidu_route(dest_name, current_lat_lon):
    """
    获取百度步行导航路线
    current_lat_lon: "纬度,经度" (WGS84坐标，GPS原始坐标)
    """
    # 先验证目的地是否有效
    is_valid, error_msg = is_valid_destination(dest_name)
    if not is_valid:
        print(f"[百度地图] 目的地验证失败: {error_msg}, 目的地: '{dest_name}'")
        return [f"目的地'{dest_name}'看起来不像有效地址，请再说一次，比如：带我去湖北师范大学"], None

    # 第一步：地理编码（地址转坐标）
    # 注意：百度地理编码返回的是百度坐标(bd09ll)
    # 使用用户设置的常住地作为城市限定，提高搜索准确性
    home_city = user_config.get_home_city()

    # 检查用户是否已经在目的地中指定了城市
    # 如果目的地包含常见城市名，则不添加常住地限定（让用户可以搜索其他城市）
    common_cities = ["北京", "上海", "广州", "深圳", "杭州", "武汉", "南京", "成都", "西安", "重庆",
                     "天津", "苏州", "长沙", "郑州", "沈阳", "青岛", "宁波", "东莞", "无锡", "黄石"]
    has_city_in_dest = any(city in dest_name for city in common_cities)

    if has_city_in_dest:
        # 用户已经指定了城市，不使用常住地限定
        geo_url = f"https://api.map.baidu.com/geocoding/v3/?address={dest_name}&ak={BAIDU_MAP_AK}&output=json"
        print(f"[百度地图] 目的地已包含城市名，直接搜索: {dest_name}")
    else:
        # 使用常住地作为城市限定
        geo_url = f"https://api.map.baidu.com/geocoding/v3/?address={dest_name}&city={home_city}&ak={BAIDU_MAP_AK}&output=json"
        print(f"[百度地图] 使用常住地'{home_city}'限定搜索: {dest_name}")

    dest_lat_lon = "39.990463,116.481488"  # 默认坐标（北京）
    try:
        geo_res = requests.get(geo_url, timeout=10).json()
        if geo_res.get("status") == 0:
            location = geo_res["result"]["location"]
            # 百度返回的是 lng,lat，我们需要 lat,lng 格式
            dest_lat_lon = f"{location['lat']},{location['lng']}"
            print(f"[百度地图] 成功解析目的地: {dest_name} -> {dest_lat_lon}")
        else:
            print(f"[百度地图] 地理编码失败: {geo_res.get('msg', '未知错误')}")
    except Exception as e:
        print(f"[百度地图] 地理编码请求失败: {e}")

    # 第二步：步行路径规划
    # coord_type=wgs84 表示输入是GPS坐标，百度会自动转换
    route_url = f"https://api.map.baidu.com/directionlite/v1/walking?origin={current_lat_lon}&destination={dest_lat_lon}&ak={BAIDU_MAP_AK}&coord_type=wgs84"

    try:
        route_res = requests.get(route_url, timeout=10).json()
        if route_res.get("status") == 0:
            routes = route_res.get("result", {}).get("routes", [])
            if len(routes) > 0:
                steps = routes[0].get("steps", [])
                instructions = []
                step_points = []  # 保存每步的起始坐标
                for idx, step in enumerate(steps):
                    instr = step.get("instruction", "")
                    distance = step.get("distance", 0)
                    # 获取步骤的起始坐标
                    step_start = step.get("step_origin_location", {})
                    step_end = step.get("step_destination_location", {})
                    if instr:
                        # 清洗HTML标签
                        import re
                        instr = re.sub(r'<[^>]+>', '', instr)  # 去除<b>等HTML标签

                        # 简化方向词，口语化表达
                        instr = instr.replace("向正南方向", "向南")
                        instr = instr.replace("向正北方向", "向北")
                        instr = instr.replace("向正东方向", "向东")
                        instr = instr.replace("向正西方向", "向西")
                        instr = instr.replace("出发，", "")
                        instr = instr.replace("左转", "向左转")
                        instr = instr.replace("右转", "向右转")

                        # 添加距离信息
                        if distance > 0:
                            instr += f"，走{distance}米"
                        instructions.append(instr)
                        # 保存坐标点用于实时导航
                        step_points.append({
                            "lat": step_start.get("lat", 0),
                            "lng": step_start.get("lng", 0),
                            "end_lat": step_end.get("lat", 0),
                            "end_lng": step_end.get("lng", 0),
                            "distance": distance
                        })
                print(f"[百度地图] 规划成功，共 {len(instructions)} 个路段")

                # 检查距离和步数，根据距离给出不同的提示
                total_distance = sum(step.get("distance", 0) for step in steps)

                # 超长距离（超过100公里）：提示距离太远
                if total_distance > 100000:
                    print(f"[百度地图] 警告：目的地距离过远（{total_distance/1000:.1f}公里），超出步行导航范围")
                    return [f"目的地'{dest_name}'距离太远（约{total_distance/1000:.0f}公里），超出步行导航范围，请选择更近的地点，比如：带我去附近的公园"], None

                # 长距离（超过10公里）：提示距离较远
                if total_distance > 10000:
                    print(f"[百度地图] 警告：目的地距离较远（{total_distance/1000:.1f}公里）")
                    return [f"目的地'{dest_name}'距离较远（约{total_distance/1000:.1f}公里），建议选择合适的交通方式后再导航，或选择附近的目的地"], None

                # 步数过多但距离适中（超过15步且超过5公里）：提示目的地不明确
                if len(instructions) > 15 and total_distance > 5000:
                    print(f"[百度地图] 警告：路线过长（{len(instructions)}步，{total_distance}米），目的地可能不准确")
                    return [f"目的地'{dest_name}'距离太远或位置不明确（约{total_distance/1000:.1f}公里），请确认具体地点，比如：带我去黄石万达广场"], None

                # 保存步骤坐标到系统状态
                system_state.nav_step_points = step_points
                return instructions, dest_lat_lon
            else:
                return ["未找到可行路线"], dest_lat_lon
        else:
            error_msg = route_res.get("message", "请求失败")
            print(f"[百度地图] 路径规划失败: {error_msg}")
            # 检查是否是距离太远的错误
            if "distance" in error_msg.lower() and ("less" in error_msg.lower() or "200" in error_msg):
                return [f"目的地'{dest_name}'距离太远，已超出步行导航范围（最大200公里），请选择更近的地点，比如：带我去附近的超市或医院"], None
            return [f"路径规划失败: {error_msg}"], dest_lat_lon
    except Exception as e:
        print(f"[百度地图] 路径规划请求失败: {e}")
        return [f"请求失败: {e}"], dest_lat_lon

# ==================== 导航步骤自动推进 ====================
import math

def haversine_distance(lat1, lng1, lat2, lng2):
    """计算两个GPS坐标之间的距离（米）"""
    R = 6371000  # 地球半径（米）
    phi1 = math.radians(lat1)
    phi2 = math.radians(lat2)
    delta_phi = math.radians(lat2 - lat1)
    delta_lambda = math.radians(lng2 - lng1)

    a = math.sin(delta_phi / 2) ** 2 + \
        math.cos(phi1) * math.cos(phi2) * math.sin(delta_lambda / 2) ** 2
    c = 2 * math.atan2(math.sqrt(a), math.sqrt(1 - a))

    return R * c


def point_to_line_distance(lat, lng, line_start_lat, line_start_lng, line_end_lat, line_end_lng):
    """
    计算点到线段的距离（米）
    使用球面几何计算，返回点到线段的垂直距离
    """
    # 如果线段起点和终点相同，直接返回点到起点的距离
    if line_start_lat == line_end_lat and line_start_lng == line_end_lng:
        return haversine_distance(lat, lng, line_start_lat, line_start_lng)

    # 将坐标转换为弧度
    lat_rad = math.radians(lat)
    lng_rad = math.radians(lng)
    line_start_lat_rad = math.radians(line_start_lat)
    line_start_lng_rad = math.radians(line_start_lng)
    line_end_lat_rad = math.radians(line_end_lat)
    line_end_lng_rad = math.radians(line_end_lng)

    # 计算线段长度（米）
    line_length = haversine_distance(line_start_lat, line_start_lng, line_end_lat, line_end_lng)

    # 计算点到线段两个端点的距离
    dist_to_start = haversine_distance(lat, lng, line_start_lat, line_start_lng)
    dist_to_end = haversine_distance(lat, lng, line_end_lat, line_end_lng)

    # 如果线段很短，返回到最近端点的距离
    if line_length < 1.0:
        return min(dist_to_start, dist_to_end)

    # 使用海伦公式计算三角形面积，然后求高
    # 三角形三边：a=dist_to_end, b=dist_to_start, c=line_length
    a = dist_to_end
    b = dist_to_start
    c = line_length

    # 检查是否能构成三角形
    if a + b <= c or a + c <= b or b + c <= a:
        return min(dist_to_start, dist_to_end)

    # 海伦公式计算面积
    s = (a + b + c) / 2  # 半周长
    area = math.sqrt(max(0, s * (s - a) * (s - b) * (s - c)))

    # 面积 = 0.5 * 底 * 高，所以 高 = 2 * 面积 / 底
    height = 2 * area / c

    # 检查垂足是否在线段上（使用余弦定理）
    # 如果角是钝角，垂足在线段外，应该返回到最近端点的距离
    cos_angle_at_start = (b**2 + c**2 - a**2) / (2 * b * c)
    cos_angle_at_end = (a**2 + c**2 - b**2) / (2 * a * c)

    if cos_angle_at_start < 0 or cos_angle_at_end < 0:
        # 垂足在线段外，返回到最近端点的距离
        return min(dist_to_start, dist_to_end)

    return height


def check_route_deviation():
    """
    检查用户是否偏离导航路线
    返回: (是否偏离, 偏离距离米)
    """
    if not system_state.nav_active:
        return False, 0

    if not system_state.nav_steps or not system_state.nav_step_points:
        return False, 0

    current_step = system_state.current_step
    step_points = system_state.nav_step_points

    # 只检查当前步骤和下一步的路线段
    steps_to_check = []

    # 当前步骤
    if current_step < len(step_points):
        steps_to_check.append(current_step)
    # 下一步（如果存在）
    if current_step + 1 < len(step_points):
        steps_to_check.append(current_step + 1)
    # 上一步（如果刚进入新步骤不久）
    if current_step > 0:
        steps_to_check.append(current_step - 1)

    min_distance = float('inf')
    closest_step = -1

    for step_idx in steps_to_check:
        if step_idx >= len(step_points):
            continue

        step_point = step_points[step_idx]
        start_lat = step_point.get("lat", 0)
        start_lng = step_point.get("lng", 0)
        end_lat = step_point.get("end_lat", 0)
        end_lng = step_point.get("end_lng", 0)

        if start_lat == 0 or end_lat == 0:
            continue

        # 计算到当前步骤线段的距离
        dist = point_to_line_distance(
            system_state.current_lat, system_state.current_lng,
            start_lat, start_lng,
            end_lat, end_lng
        )

        if dist < min_distance:
            min_distance = dist
            closest_step = step_idx

    # 偏离阈值：30米
    DEVIATION_THRESHOLD = 30.0

    is_deviated = min_distance > DEVIATION_THRESHOLD

    if is_deviated:
        print(f"[路线偏离检测] 偏离距离: {min_distance:.1f}米, 阈值: {DEVIATION_THRESHOLD}米, 最近步骤: {closest_step + 1}")

    return is_deviated, min_distance

def handle_route_deviation():
    """
    处理路线偏离：增加路线调整计数，并播报提醒
    """
    print("[路线偏离] 检测到用户偏离原路线，增加路线调整计数")

    # 增加路线调整计数
    system_state.stats["detour_count"] += 1
    detour_count = system_state.stats["detour_count"]

    # 播报偏离提醒
    deviation_msg = "您已偏离原路线，正在重新规划"
    tts_queue.add(deviation_msg, priority=3)
    broadcast_to_clients({
        "type": "tts",
        "text": deviation_msg
    })

    # 广播路线调整事件
    broadcast_to_clients({
        "type": "route_deviation",
        "detour_count": detour_count,
        "message": "路线已调整",
        "current_lat": system_state.current_lat,
        "current_lng": system_state.current_lng
    })

    # 添加事件记录
    add_event_log("导航", f"路线调整 #{detour_count}: 用户偏离原路线")

    print(f"[路线偏离] 当前路线调整次数: {detour_count}")
    return detour_count


def check_and_advance_step():
    """
    根据当前GPS位置检查是否需要推进到下一步导航
    当距离当前步骤终点小于阈值时，自动进入下一步
    """
    if not system_state.nav_active:
        return

    # 检查是否有有效的步骤数据
    if not system_state.nav_steps or not system_state.nav_step_points:
        return

    # 检查是否偏离路线（每10秒最多检查一次，避免频繁触发）
    current_time = time.time()
    last_deviation_check = getattr(system_state, 'last_deviation_check', 0)
    if current_time - last_deviation_check > 10:
        system_state.last_deviation_check = current_time
        is_deviated, deviation_dist = check_route_deviation()

        if is_deviated:
            # 检查是否已经偏离了一段时间（避免GPS抖动误触发）
            last_deviation_time = getattr(system_state, 'last_deviation_time', 0)
            if current_time - last_deviation_time > 30:  # 30秒内不重复触发
                system_state.last_deviation_time = current_time
                handle_route_deviation()
            else:
                print(f"[路线偏离] 检测到偏离 {deviation_dist:.1f}米，但处于冷却期内，暂不处理")

    current_step = system_state.current_step
    step_points = system_state.nav_step_points
    steps = system_state.nav_steps

    # 步骤数据不匹配，重置导航
    if len(step_points) != len(steps):
        print(f"[导航] 步骤数据不匹配: steps={len(steps)}, points={len(step_points)}")
        return

    if current_step >= len(step_points) or current_step >= len(steps):
        # 导航完成
        if system_state.nav_active:
            system_state.nav_active = False
            system_state.nav_step_text = "已到达目的地"
            print(f"[导航] 已到达目的地: {system_state.nav_destination}")
            # 播报到达（添加到队列）
            arrival_msg = f"已到达{system_state.nav_destination}附近，导航结束"
            tts_queue.add(arrival_msg, priority=1)
            # 通过广播通知前端
            broadcast_to_clients({
                "type": "tts",
                "text": arrival_msg
            })
        return

    # 获取当前步骤的终点坐标
    current_step_point = step_points[current_step]
    end_lat = current_step_point.get("end_lat", 0)
    end_lng = current_step_point.get("end_lng", 0)

    if end_lat == 0 or end_lng == 0:
        return

    # 计算当前位置到步骤终点的距离
    distance_to_end = haversine_distance(
        system_state.current_lat, system_state.current_lng,
        end_lat, end_lng
    )

    # 如果距离小于阈值（15米），认为到达该步骤，进入下一步
    threshold = system_state.step_threshold
    if distance_to_end < threshold:
        next_step = current_step + 1
        if next_step < len(steps):
            system_state.current_step = next_step
            next_instruction = steps[next_step]
            system_state.nav_step_text = next_instruction
            print(f"[导航] 步骤推进: {current_step + 1} -> {next_step + 1}, 距离终点{distance_to_end:.1f}米")
            print(f"[导航] 下一步: {next_instruction}")

            # 记录导航语音播报时间和预估时长（每字约0.3秒）
            system_state.last_nav_speak_time = time.time()
            system_state.nav_speak_duration = max(3, len(next_instruction) * 0.3)
            # 播报下一步指令（添加到队列，普通优先级）
            tts_queue.add(next_instruction, priority=1)
            broadcast_to_clients({
                "type": "tts",
                "text": next_instruction
            })
        else:
            # 导航完成
            system_state.nav_active = False
            system_state.nav_step_text = "已到达目的地"
            print(f"[导航] 已到达目的地: {system_state.nav_destination}")
            # 广播导航完成状态
            broadcast_to_clients({
                "type": "state",
                "nav_active": False,
                "nav_destination": system_state.nav_destination,
                "nav_step": "已到达目的地",
                "nav_steps": [],
                "current_step": 0
            })

# ==================== WebSocket广播 ====================
def broadcast_to_clients(message):
    """广播消息到所有WebSocket客户端"""
    if not web_clients:
        return

    payload = json.dumps(message)
    removable = set()

    for client in web_clients:
        try:
            client.send(payload)
        except Exception:
            removable.add(client)

    # 清理断开的连接
    for client in removable:
        web_clients.discard(client)

# ==================== 障碍物处理逻辑 ====================
def process_obstacle_alert():
    """
    根据雷达和视觉检测决定是否触发语音告警
    返回: (alert_text, motor_command)
    alert_text: 语音播报文本
    motor_command: 电机控制指令 {"action": "left"/"right"/"stop", "power": 0-255}
    """
    current_time = time.time()

    # 检查冷却时间 (3秒内不重复播报)
    if current_time - system_state.last_alert_time < 3:
        return None, None

    alert_text = None
    motor_cmd = None

    # 雷达距离阈值 - 用户要求：<180cm 即告警
    ALERT_DIST = 180.0   # 播报阈值（<180cm 播报）
    WARNING_DIST = 180.0  # 警告距离

    front = system_state.radar_front
    left = system_state.radar_left
    right = system_state.radar_right

    print(f"[障碍物检查] 雷达: 前={front:.0f}cm, 左={left:.0f}cm, 右={right:.0f}cm, 阈值={ALERT_DIST}cm")

    # 判断障碍物方位和生成播报文本
    # 优先级：正前方危险(<180cm) > 左侧/右侧(<180cm)

    if front < ALERT_DIST:
        # 前方危险，需要紧急避让
        if left > right:
            alert_text = f"前方{int(front)}厘米有障碍物，请立即向左避让"
            motor_cmd = {"action": "left", "power": 255}
        else:
            alert_text = f"前方{int(front)}厘米有障碍物，请立即向右避让"
            motor_cmd = {"action": "right", "power": 255}

    elif left < ALERT_DIST and left <= right:
        # 左侧有障碍（<180cm 即播报），建议靠右
        alert_text = f"左侧{int(left)}厘米有障碍物，建议靠右避让"
        motor_cmd = {"action": "right", "power": 128}

    elif right < ALERT_DIST and right < left:
        # 右侧有障碍（<180cm 即播报），建议靠左
        alert_text = f"右侧{int(right)}厘米有障碍物，建议靠左避让"
        motor_cmd = {"action": "left", "power": 128}

    # 如果有视觉检测结果，补充具体障碍物类型（只使用置信度>=0.4的检测结果）
    if system_state.latest_detections and alert_text:
        # 过滤出置信度>=0.4的检测结果
        valid_detections = [det for det in system_state.latest_detections if det.get("confidence", 0) >= 0.4]

        if valid_detections:
            # 找到最靠近的检测目标（按置信度最高）
            closest_det = max(valid_detections, key=lambda det: det.get("confidence", 0))
            label = closest_det.get("label", "")
            obj_class = closest_det.get("class", "")  # 获取英文类别名
            distance = closest_det.get("distance")

            if label:
                # 定义危险障碍物（需要播报"危险"前缀）
                DANGEROUS_OBJECTS = {"vehicle", "puddle"}  # 车辆、水坑是危险障碍物
                is_dangerous = obj_class in DANGEROUS_OBJECTS

                # 调试输出
                print(f"[障碍物检测] label={label}, class={obj_class}, is_dangerous={is_dangerous}")

                # 根据是否是危险物体，决定是否添加"危险"前缀
                if is_dangerous:
                    # 危险障碍物：添加"危险"前缀
                    alert_text = alert_text.replace("障碍物", f"危险{label}")
                else:
                    # 普通物体：直接替换，不加"危险"
                    alert_text = alert_text.replace("障碍物", label)

    # 如果检测到障碍物且正在导航中，标记导航被打断
    if alert_text and system_state.nav_active:
        system_state.nav_interrupted = True
        print(f"[导航] 被障碍物打断，设置 nav_interrupted=True")

    if alert_text:
        system_state.last_voice_alert = alert_text
        system_state.last_alert_time = current_time
        print(f"[障碍物告警] {alert_text}")

    return alert_text, motor_cmd

# ==================== 1. 静态网页托管功能 ====================
@app.route('/')
def index():
    """根路径：直接返回大屏幕首页 index.html"""
    if os.path.exists(os.path.join(STATIC_FOLDER, 'index.html')):
        return send_from_directory(STATIC_FOLDER, 'index.html')
    else:
        return f"<h3>[错误] 未在大端路径找到网页文件！</h3>请确保 <b>{STATIC_FOLDER}</b> 目录下存在 index.html", 404

@app.route('/<path:path>')
def serve_static(path):
    """托管 css/, js/ 等外部静态资源"""
    return send_from_directory(STATIC_FOLDER, path)

# ==================== 2. 接收导盲杖硬件数据接口 (ESP32雷达/GPS + K230视觉检测) ====================
@app.route('/api/data', methods=['POST'])
def receive_device_data():
    """
    统一接收来自 ESP32-S3 和 K230 的数据
    - ESP32: 包含 radar, gps, nav_step, blocked, status
    - K230: 包含 detections, img_size
    """
    try:
        data = request.get_json()
        if not data:
            return jsonify({"status": "error", "message": "No JSON payload"}), 400

        # 🚀 优先检查是否有缓存的障碍物TTS音频待投递
        # 如果有，直接返回音频（不阻塞ESP32的轮询），后台处理本次数据
        with _pending_audio["lock"]:
            if _pending_audio["data"] is not None:
                alert_text_cached, wav, priority = _pending_audio["data"]
                _pending_audio["data"] = None  # 取走即清空
                response = Response(wav, mimetype="audio/wav")
                response.headers["X-TTS-Priority"] = str(priority)
                response.headers["X-TTS-Type"] = "obstacle_alert"
                print(f"[TTS投递] ✅ 投递缓存的障碍物音频: '{alert_text_cached[:20]}...' ({len(wav)}字节) 优先级={priority}")
                return response

        # 判断数据来源
        has_radar = "radar" in data
        has_detections = "detections" in data

        # ========== 处理 ESP32 数据 ==========
        if has_radar:
            # 更新雷达数据
            radar = data["radar"]
            system_state.radar_front = radar.get("front", 400.0)
            system_state.radar_left = radar.get("left", 400.0)
            system_state.radar_right = radar.get("right", 400.0)

            # 更新GPS数据
            if "gps" in data:
                gps = data["gps"]
                system_state.current_lat = gps.get("lat", 0.0)
                system_state.current_lng = gps.get("lng", 0.0)
                system_state.current_speed = gps.get("speed", 0.0)
                system_state.gps_satellites = gps.get("satellites", 0)
                system_state.gps_valid = system_state.gps_satellites > 0

                # 导航中：根据GPS位置自动推进步骤
                if system_state.nav_active and system_state.nav_step_points:
                    check_and_advance_step()

            # 更新导航状态
            if "nav_step" in data:
                system_state.nav_step_text = data["nav_step"]

            if "blocked" in data:
                system_state.is_blocked = data["blocked"]

            # 更新设备状态
            if "status" in data:
                system_state.esp32_online = data["status"].get("esp32", False)

            system_state.esp32_online = True
            print(f"[ESP32] 雷达 F:{system_state.radar_front:.0f} L:{system_state.radar_left:.0f} R:{system_state.radar_right:.0f} GPS:{system_state.gps_satellites}星")

        # ========== 处理 K230 视觉检测数据 ==========
        if has_detections:
            detections = data.get("detections", [])
            system_state.latest_detections = detections
            system_state.last_detection_time = time.time()
            system_state.k230_online = True
            print(f"[K230] 检测到 {len(detections)} 个目标")

        # ========== 障碍物告警检查 ==========
        alert_text, motor_cmd = process_obstacle_alert()

        # ========== WebSocket广播 ==========
        # 包装成网页 app.js 需要的 type: 'state' 数据格式
        ws_message = {
            "type": "state",
            "data": data
        }

        # 如果有雷达数据，添加雷达字段（前端需要）
        if has_radar:
            ws_message["radar"] = data["radar"]
            # 确保GPS数据也包含在顶层（方便前端读取）
            if "gps" in data:
                ws_message["gps"] = data["gps"]

        # 添加导航状态（从前端显示）
        ws_message["nav_active"] = system_state.nav_active
        ws_message["nav_destination"] = system_state.nav_destination
        ws_message["nav_step"] = system_state.nav_step_text
        ws_message["current_step"] = system_state.current_step
        # 只有导航激活时才发送步骤列表，避免显示旧数据
        if system_state.nav_active:
            ws_message["nav_steps"] = system_state.nav_steps
        else:
            ws_message["nav_steps"] = []

        # 如果有障碍物告警，也添加到消息中并记录事件
        if alert_text:
            ws_message["alert"] = alert_text
            # 添加事件记录
            add_event_log("障碍物", alert_text)

        # 广播给所有打开了网页的浏览器
        print(f"[广播] WebSocket消息: radar={ws_message.get('radar')}, nav_active={ws_message.get('nav_active')}")
        broadcast_to_clients(ws_message)

        # ========== 返回障碍物告警（TTS语音 + 电机指令）==========
        response_data = {
            "status": "success",
            "alert": alert_text,
            "nav_active": system_state.nav_active  # ESP32同步导航状态
        }
        if motor_cmd:
            response_data["motor"] = motor_cmd

        # 如果有障碍物告警 — 障碍物优先级高于导航，立即打断导航播报
        if alert_text:
            print(f"[告警] 生成告警: {alert_text}")

            # 🚀 障碍物优先级最高(2)，立即打断导航语音
            response_data["tts_pending"] = True
            response_data["tts_priority"] = 2  # 高优先级，ESP32会打断当前播放
            threading.Thread(target=_synthesize_and_cache_alert, args=(alert_text, 2), daemon=True).start()

        return jsonify(response_data)

    except Exception as e:
        print(f"[设备数据] 处理错误: {e}")
        return jsonify({"status": "error", "message": str(e)}), 500

# ==================== 3. 接收K230视觉检测数据 (兼容旧版单独接口) ====================
@app.route('/api/k230/detections', methods=['POST'])
def receive_k230_detections():
    """接收来自 K230 的视觉检测结果 (兼容旧版单独上报)"""
    try:
        data = request.get_json()
        detections = data.get("detections", [])

        # 更新状态
        system_state.latest_detections = detections
        system_state.last_detection_time = time.time()
        system_state.k230_online = True

        # 广播到前端
        broadcast_to_clients({
            "type": "detections",
            "data": detections
        })

        print(f"[K230/单独] 检测到 {len(detections)} 个目标")
        return jsonify({"status": "success", "detection_count": len(detections)})

    except Exception as e:
        print(f"[K230] 数据处理错误: {e}")
        return jsonify({"status": "error", "message": str(e)}), 500

# ==================== 4. 语音交互接口 (百度ASR + 通义千问 + 百度TTS) ====================
@app.route('/api/chat', methods=['POST'])
def receive_audio_chat():
    """
    接收硬件PCM音频，进行语音识别 -> 唤醒词检测 -> LLM对话 -> 语音合成
    返回 WAV 音频
    """
    try:
        # 获取PCM音频数据
        pcm_data = request.data

        if len(pcm_data) < 1000:
            return jsonify({"status": "error", "message": "Audio too short"}), 400

        print(f"[语音] 收到音频数据: {len(pcm_data)} 字节")

        # 1. 语音识别
        user_text = baidu_service.asr(pcm_data)
        if not user_text:
            return jsonify({"status": "error", "message": "ASR failed"}), 400

        print(f"[ASR] 识别结果: '{user_text}'")

        # 🌟 不再单独播放"收到XXX"确认音，直接处理结果
        # 避免确认音和回复音同时播放

        # 2. 如果正在导航中，忽略新的语音输入（专注于当前导航）
        if system_state.nav_active:
            print(f"[语音] 当前正在导航到 {system_state.nav_destination}，忽略新的语音指令")
            return jsonify({"status": "ignored", "message": "Navigation in progress"}), 200

        # 3. 检查是否是导航指令（必须以导航触发词开头，或触发词在句首位置）
        # 🌟 严格匹配：只匹配句首的导航意图，避免中间出现"去"等字被误判
        is_nav_cmd = False
        matched_trigger = ""
        for trigger in NAV_TRIGGERS:
            # 检查是否以触发词开头，或触发词前有停顿词（如"嗯"、"啊"等）
            if user_text.startswith(trigger):
                is_nav_cmd = True
                matched_trigger = trigger
                break
            # 也允许在触发词前有 "嗯"、"啊" 等语气词（如"嗯我要去天安门"）
            elif any(user_text.startswith(filler + trigger) for filler in ["嗯", "啊", "哦", "那个"]):
                is_nav_cmd = True
                matched_trigger = trigger
                break

        if not is_nav_cmd:
            print(f"[语音] 未检测到导航指令（必须以'带我去/我要去/要去/去'等开头），忽略: '{user_text}'")
            return jsonify({"status": "ignored", "message": "Not a nav command"}), 400
        else:
            print(f"[语音] 检测到导航指令，触发词: '{matched_trigger}'")

        # 🌟 检查原始文本是否包含明显不是目的地的内容（疑问句、乱码等）
        if any(q in user_text for q in ['为什么', '是什么', '怎么', '哪里', '什么', '这句话', '识别', '差不多', '认识']):
            reply = f"收到{user_text}，但这听起来不像目的地，请再说一次，比如：带我去天安门"
            print(f"[导航] 原始文本包含非目的地词汇，拒绝导航: '{user_text}'")
            # 记录对话但不导航
            add_chat_log(user_text, reply)
            broadcast_to_clients({
                "type": "chat",
                "user": user_text,
                "system": reply
            })
            wav = baidu_service.tts(reply)
            if wav:
                # 对话回复为中优先级 (1)
                response = Response(wav, mimetype="audio/wav")
                response.headers["X-TTS-Priority"] = "1"  # 中优先级：对话回复
                response.headers["X-TTS-Type"] = "chat_reply"
                print(f"[对话] 返回TTS音频，优先级=中(1)，类型=对话回复")
                return response
            return jsonify({"status": "ignored", "message": "Not a valid destination"}), 400

        # 4. 清洗文本：移除导航触发词和过滤词
        dest = user_text
        # 先移除导航触发词
        for trigger in NAV_TRIGGERS:
            dest = dest.replace(trigger, "")
        # 再移除无意义词汇
        for word in FILTER_WORDS:
            dest = dest.replace(word, "")
        # 最后去除标点
        dest = dest.strip(" ，。？?!.")

        nav_rejected = False  # 标记是否被拒绝导航
        if dest:
            # 🌟 检查是否包含数字（避免把避障语音"27厘米"当目的地）
            import re
            if re.search(r'\d+', dest):
                reply = f"收到{user_text}，但这听起来不像目的地，请再说一次"
                print(f"[导航] 目的地包含数字，可能是回声: '{dest}'，拒绝导航")
                nav_rejected = True
            # 🌟 检查是否包含有效的地点后缀（避免"里面"、"外面"等无效目的地）
            # 短地名（2-3个字）放宽检查，让省份/城市名进入路线规划
            elif len(dest) > 3 and not any(suffix in dest for suffix in VALID_LOCATION_SUFFIXES):
                reply = f"收到{user_text}，但这不像一个具体地点，请说明具体位置，比如：带我去湖北师范大学"
                print(f"[导航] 目的地缺少有效地点后缀: '{dest}'，拒绝导航")
                nav_rejected = True
            # 🌟 检查是否包含疑问词或标点（避免误识别）
            elif any(q in dest for q in ['为什么', '是什么', '怎么', '哪里', '吗', '呢', '？', '！', '问号', '感叹']):
                reply = f"收到{user_text}，但这听起来不像目的地，请再说一次，比如：带我去天安门"
                print(f"[导航] 目的地包含疑问词或标点，不是有效目的地: '{dest}'，拒绝导航")
                nav_rejected = True
            else:
                # 获取当前位置 (百度需要 "纬度,经度" 格式)
                current_pos = f"{system_state.current_lat},{system_state.current_lng}"
                if system_state.current_lat == 0:
                    current_pos = "30.231930,115.057910"  # 默认位置 (湖北黄石/湖北师范大学附近)

                print(f"[导航] 清洗后的目的地: '{dest}'")

                # 获取导航路线
                steps, dest_coords = get_baidu_route(dest, current_pos)

                # 检查路径规划是否成功
                if len(steps) <= 1 or "失败" in steps[0] or "未找到" in steps[0]:
                    # 检查是否是距离太远的提示
                    if "距离太远" in steps[0] or "距离较远" in steps[0]:
                        reply = steps[0]  # 使用路线规划返回的提示
                        print(f"[导航] 距离判断拒绝: {steps[0]}")
                    else:
                        reply = f"收到，但路径规划失败，请检查目的地是否正确"
                        print(f"[导航] 路径规划失败: {steps}")
                    nav_rejected = True
                    # 不设置nav_active，让用户可以重新输入
                else:
                    system_state.nav_steps = steps
                    system_state.nav_destination = dest
                    system_state.nav_active = True
                    system_state.current_step = 0
                    # 播报总体信息+第一步详细内容
                    first_step = steps[0] if len(steps) > 0 else ""
                    reply = f"开始导航到{dest}，共{len(steps)}个路段。第一步，{first_step}"
                    # 记录导航历史
                    add_nav_log(dest, len(steps))
                    print(f"[导航] 开始导航到 {dest}")
        else:
            reply = f"收到{user_text}，但没听清目的地，请再说一次，比如：带我去天安门"
            print(f"[回复] '{reply}'")
            nav_rejected = True

        # 6. 广播对话到前端并记录
        add_chat_log(user_text, reply)
        broadcast_to_clients({
            "type": "chat",
            "user": user_text,
            "system": reply
        })

        # 7. 语音合成 - 使用队列避免冲突
        print(f"[TTS] 合成导航语音: '{reply}'")
        # 记录导航语音播报时间，用于避障冲突检测
        system_state.last_nav_speak_time = time.time()
        # 根据文本长度计算预估播放时长（每字约0.3秒）
        system_state.nav_speak_duration = max(5, len(reply) * 0.3)
        # 添加到队列，最高优先级(3) - 用户主动发起的导航指令应打断避障语音
        tts_queue.add(reply, priority=3)
        wav = baidu_service.tts(reply)
        if wav:
            print(f"[TTS] 合成成功，返回音频给ESP32播放，大小: {len(wav)} 字节")
            # 导航语音为最高优先级 (3)
            response = Response(wav, mimetype="audio/wav")
            response.headers["X-TTS-Priority"] = "3"  # 最高优先级：用户导航指令
            response.headers["X-TTS-Type"] = "navigation"
            # 根据是否被拒绝设置导航状态
            response.headers["X-Nav-Active"] = "0" if nav_rejected else "1"
            print(f"[TTS] 返回导航语音，优先级=最高(3)，类型=导航，导航状态={'拒绝' if nav_rejected else '开始'}")
            print(f"[TTS] 响应Content-Type: {response.content_type}")
            return response
        else:
            print(f"[TTS] 合成失败！")

        return jsonify({"status": "error", "message": "TTS failed"}), 500

    except Exception as e:
        print(f"[语音] 处理错误: {e}")
        return jsonify({"status": "error", "message": str(e)}), 500

# ==================== 5. TTS文本转语音接口 ====================
@app.route('/api/tts', methods=['POST'])
def text_to_speech():
    """纯文本转语音接口"""
    try:
        data = request.get_json()
        text = data.get("text", "")

        if not text:
            return jsonify({"error": "缺少text参数"}), 400

        print(f"[TTS] 合成: '{text}'")

        wav = baidu_service.tts(text)
        if wav:
            # 广播到前端
            broadcast_to_clients({
                "type": "tts",
                "text": text
            })
            return Response(wav, mimetype="audio/wav")

        return jsonify({"error": "TTS失败"}), 500

    except Exception as e:
        print(f"[TTS] 处理错误: {e}")
        return jsonify({"error": str(e)}), 500

# ==================== 6. 高德导航接口 ====================
@app.route('/api/navigation', methods=['POST'])
def start_navigation():
    """开始导航"""
    try:
        data = request.get_json()
        dest = data.get("destination", "")

        if not dest:
            return jsonify({"error": "缺少destination参数"}), 400

        # 获取当前位置 (百度需要 "纬度,经度" 格式)
        current_pos = f"{system_state.current_lat},{system_state.current_lng}"
        if system_state.current_lat == 0:
            current_pos = "30.231930,115.057910"  # 默认位置 (湖北黄石/湖北师范大学附近)

        steps, dest_coords = get_baidu_route(dest, current_pos)

        system_state.nav_steps = steps
        system_state.nav_destination = dest
        system_state.nav_active = True
        system_state.current_step = 0

        # 广播导航开始
        broadcast_to_clients({
            "type": "navigation",
            "destination": dest,
            "steps": steps
        })

        return jsonify({
            "status": "ok",
            "destination": dest,
            "steps_count": len(steps),
            "steps": steps
        })

    except Exception as e:
        print(f"[导航] 处理错误: {e}")
        return jsonify({"error": str(e)}), 500

# ==================== 7. 获取导航路线接口（供ESP32调用）====================
@app.route('/api/nav_steps', methods=['GET'])
def get_nav_steps():
    """ESP32获取当前导航路线步骤"""
    return jsonify({
        "status": "ok" if system_state.nav_active else "waiting",
        "destination": system_state.nav_destination,
        "steps": system_state.nav_steps,
        "current_step": system_state.current_step,
        "total_steps": len(system_state.nav_steps)
    })

# ==================== 7.5 停止导航接口 ====================
@app.route('/api/navigation/stop', methods=['POST'])
def stop_navigation():
    """停止当前导航"""
    try:
        system_state.nav_active = False
        system_state.nav_destination = ""
        system_state.nav_steps = []
        system_state.nav_step_points = []
        system_state.current_step = 0
        system_state.nav_step_text = "等待导航开始"
        print("[导航] 用户手动停止导航")

        # 🌟 广播导航停止状态给所有客户端（网页端和ESP32）
        broadcast_to_clients({
            "type": "state",
            "nav_active": False,
            "nav_destination": "",
            "nav_step": "等待导航开始",
            "nav_steps": [],
            "current_step": 0,
            "message": "导航已手动停止"
        })

        return jsonify({"status": "ok", "message": "导航已停止"})
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

# ==================== 8. 系统状态查询接口 ====================
@app.route('/api/status', methods=['GET'])
def get_status():
    """获取系统状态"""
    return jsonify(system_state.to_dict())

# ==================== 8.2 事件记录接口 ====================
@app.route('/api/events', methods=['GET'])
def get_events():
    """获取事件记录"""
    try:
        limit = request.args.get('limit', 50, type=int)
        return jsonify({
            "status": "ok",
            "events": get_event_logs(limit)
        })
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

@app.route('/api/events/clear', methods=['POST'])
def clear_events():
    """清空事件记录"""
    try:
        system_state.event_logs = []
        return jsonify({"status": "ok", "message": "事件记录已清空"})
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

# ==================== 8.3 语音对话记录接口 ====================
@app.route('/api/chat_logs', methods=['GET'])
def get_chat_logs_api():
    """获取语音对话记录"""
    try:
        limit = request.args.get('limit', 50, type=int)
        return jsonify({
            "status": "ok",
            "chats": get_chat_logs(limit)
        })
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

@app.route('/api/chat_logs/clear', methods=['POST'])
def clear_chat_logs():
    """清空语音对话记录"""
    try:
        system_state.chat_logs = []
        return jsonify({"status": "ok", "message": "语音对话记录已清空"})
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

# ==================== 8.4 导航记录接口 ====================
@app.route('/api/nav_logs', methods=['GET'])
def get_nav_logs_api():
    """获取导航记录"""
    try:
        limit = request.args.get('limit', 20, type=int)
        return jsonify({
            "status": "ok",
            "navs": get_nav_logs(limit)
        })
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

@app.route('/api/nav_logs/clear', methods=['POST'])
def clear_nav_logs():
    """清空导航记录"""
    try:
        system_state.nav_history = []
        return jsonify({"status": "ok", "message": "导航记录已清空"})
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

# ==================== 8.45 统计数据接口 ====================
@app.route('/api/stats', methods=['GET'])
def get_stats_api():
    """获取统计数据"""
    try:
        return jsonify({
            "status": "ok",
            "stats": get_stats()
        })
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

@app.route('/api/stats', methods=['POST'])
def update_stats_api():
    """更新统计数据"""
    try:
        data = request.get_json()
        if not data:
            return jsonify({"status": "error", "message": "No JSON payload"}), 400

        update_stats(
            nav_count=data.get("nav_count"),
            obstacle_count=data.get("obstacle_count"),
            detour_count=data.get("detour_count"),
            mileage=data.get("mileage")
        )
        return jsonify({"status": "ok", "stats": get_stats()})
    except Exception as e:
        return jsonify({"status": "error", "message": str(e)}), 500

# ==================== 8.5 用户配置接口（常住地设置）====================
@app.route('/api/config', methods=['GET', 'OPTIONS'])
def get_config():
    """获取用户配置"""
    if request.method == 'OPTIONS':
        return jsonify({"status": "ok"})
    return jsonify({
        "status": "ok",
        "home_city": user_config.get_home_city()
    })

@app.route('/api/config', methods=['POST', 'OPTIONS'])
def set_config():
    """设置用户配置（常住地）"""
    if request.method == 'OPTIONS':
        return jsonify({"status": "ok"})
    try:
        data = request.get_json()
        if not data:
            return jsonify({"status": "error", "message": "No JSON payload"}), 400

        home_city = data.get("home_city", "").strip()
        if not home_city:
            return jsonify({"status": "error", "message": "常住地不能为空"}), 400

        # 自动添加"市"后缀（如果没有的话）
        if not home_city.endswith("市") and not home_city.endswith("区") and not home_city.endswith("县"):
            home_city += "市"

        if user_config.set_home_city(home_city):
            return jsonify({
                "status": "ok",
                "message": f"常住地已设置为：{home_city}",
                "home_city": home_city
            })
        else:
            return jsonify({"status": "error", "message": "保存配置失败"}), 500

    except Exception as e:
        print(f"[配置] 设置配置失败: {e}")
        return jsonify({"status": "error", "message": str(e)}), 500

# ==================== 10. WebSocket 服务 ====================
@sock.route('/ws')
def handle_ws(ws):
    """网页端通过此通道维持实时刷新大屏"""
    print("[WS] 网页端大屏客户端已成功建立连接!")
    web_clients.add(ws)

    # 发送当前状态
    try:
        ws.send(json.dumps({
            "type": "state",
            "data": system_state.to_dict()
        }))
    except Exception as e:
        print(f"[WS] 发送初始状态失败: {e}")

    while True:
        try:
            msg = ws.receive()
            # 可以处理前端发来的控制指令
        except Exception:
            web_clients.discard(ws)
            print("[WS] 网页端大屏客户端已断开连接。")
            break

# ==================== 11. 启动主程序 ====================
if __name__ == '__main__':
    # 固定服务器IP地址
    LOCAL_IP = "192.168.3.24"

    print("\n" + "="*70)
    print(" 🚀 AI智能导盲杖中枢服务器 启动成功！")
    print("="*70)
    print(f" 🔗 [本地大屏链接] 点击访问:  http://localhost:{PORT}")
    print(f" 🔗 [局域网大屏链接] 点击访问: http://{LOCAL_IP}:{PORT}")
    print("-"*70)
    print(" 📡 API接口:")
    print(f"    - ESP32数据上报:  POST http://{LOCAL_IP}:{PORT}/api/data")
    print(f"    - K230检测上报:   POST http://{LOCAL_IP}:{PORT}/api/k230/detections")
    print(f"    - 语音对话:       POST http://{LOCAL_IP}:{PORT}/api/chat")
    print(f"    - TTS合成:        POST http://{LOCAL_IP}:{PORT}/api/tts")
    print(f"    - 开始导航:       POST http://{LOCAL_IP}:{PORT}/api/navigation")
    print(f"    - 状态查询:       GET  http://{LOCAL_IP}:{PORT}/api/status")
    print(f"    - 获取配置:       GET  http://{LOCAL_IP}:{PORT}/api/config")
    print(f"    - 设置常住地:     POST http://{LOCAL_IP}:{PORT}/api/config")
    print("-"*70)
    print(f" 📂 当前识别到的网页根目录: {STATIC_FOLDER}")
    print("="*70 + "\n")

    app.run(host='0.0.0.0', port=PORT, debug=False)
