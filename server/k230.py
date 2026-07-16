'''
实验名称：14类物体检测（YOLOv8）+ 官方 AIBase 骨架（完美融合版）
运行说明：
  1. 适配 14 类自定义目标检测模型。
  2. 完美重写 NMS 算法，避开 ulab 库 Bug。
  3. 集成 WiFi 联网及异步 HTTP 结果上报。
  4. 集成串口输出，将检测结果发送给 ESP32。
'''

from libs.PipeLine import PipeLine, ScopedTiming
from libs.AIBase import AIBase
from libs.AI2D import Ai2d
import os
import ujson
from media.media import *
from media.sensor import *
import nncase_runtime as nn
import ulab.numpy as np
import time
import image
import gc
import sys

try:
    import _thread
except ImportError:
    _thread = None

# ==================== 📡 0. 串口输出配置（发送给 ESP32）====================
try:
    from machine import UART
    K230_UART_ID = 1          # UART1（TX=IO5, RX=IO6 — 按实际接线调整）
    K230_UART_BAUD = 115200
    K230_UART_TX_PIN = 5      # 默认 IO5
    K230_UART_RX_PIN = 6      # 默认 IO6
    uart = UART(K230_UART_ID, K230_UART_BAUD, tx=K230_UART_TX_PIN, rx=K230_UART_RX_PIN)
    K230_UART_AVAILABLE = True
    print(f"[串口] UART{K230_UART_ID} 初始化 TX=IO{K230_UART_TX_PIN} RX=IO{K230_UART_RX_PIN} 波特={K230_UART_BAUD}")
except Exception as e:
    K230_UART_AVAILABLE = False
    print(f"[串口] 初始化失败: {e}，仅 WiFi 上报模式")

# ==================== 📡 1. WiFi 联网与数据上报配置 ====================
import network
import socket

WIFI_SSID     = "Edian"
WIFI_PASSWORD = "Edian1234567890#"
SERVER_HOST   = "192.168.3.24"
SERVER_PORT   = 8080

wlan = network.WLAN(network.STA_IF)
try: wlan.active(True)
except: pass

print("[WiFi] 正在连接...")
wlan.connect(WIFI_SSID, WIFI_PASSWORD)
retry = 0
while retry < 20 and not wlan.isconnected():
    time.sleep(0.5)
    retry += 1

if wlan.isconnected():
    print("[WiFi] 成功获取 IP:", wlan.ifconfig()[0])

# 【更新】14类特定场景字典定义
LABEL_MAP = {
    'blind_track': ("盲道", "#00d4ff"),
    'curb': ("马路牙子", "#7bed9f"),
    'crosswalk': ("斑马线", "#ffffff"),
    'pole': ("立柱", "#1e90ff"),
    'ashcan': ("垃圾桶", "#747d8c"),
    'reflective_cone': ("反光锥", "#ffa502"),
    'red_light': ("红灯", "#ff4757"),
    'yellow_light': ("黄灯", "#ffa502"),
    'green_light': ("绿灯", "#2ed573"),
    'stop_sign': ("标志牌", "#ff4757"),
    'person': ("行人", "#ff4757"),
    'vehicle': ("车辆", "#ff6348"),
    'stairs': ("楼梯台阶", "#ced6e0"),
    'puddle': ("水坑", "#1e90ff"),
}

def upload_detections(detections):
    if not wlan.isconnected() or not detections: return
    try:
        payload = ujson.dumps({
            "device_id": "k230_base",
            "timestamp": time.ticks_ms(),
            "detections": detections[:5],
            "img_size": [320, 320],
        })
        addr = socket.getaddrinfo(SERVER_HOST, SERVER_PORT)[0][-1]
        s = socket.socket()
        s.settimeout(1.0)
        s.connect(addr)
        payload_bytes = payload.encode('utf-8')
        request = ("POST /api/data HTTP/1.1\r\nHost: {}:{}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nConnection: close\r\n\r\n").format(SERVER_HOST, SERVER_PORT, len(payload_bytes))
        s.send(request.encode('utf-8') + payload_bytes)
        s.close()
    except:
        pass

    # 同时通过串口发送给 ESP32
    send_detections_uart(detections)


def send_detections_uart(detections):
    """
    通过 UART 将检测结果发送给 ESP32
    单目标简单格式：DET:类名\n
    多目标扩展格式：DETS:类1,置信度1,x1,y1,w1,h1;类2,置信度2,x2,y2,w2,h2\n
    无检测：NONE\n
    """
    if not K230_UART_AVAILABLE: return
    try:
        if not detections or len(detections) == 0:
            line = "NONE\n"
        elif len(detections) == 1:
            # 单目标：简单格式 DET:类名
            cls = detections[0].get("class", "")
            line = "DET:" + cls + "\n"
        else:
            # 多目标：扩展格式 DETS:class,conf,cx,cy,w,h;...
            parts = []
            for d in detections[:5]:  # 最多5个
                cls = d.get("class", "")
                conf = d.get("confidence", 0)
                cx = d.get("x", 0)
                cy = d.get("y", 0)
                w = d.get("w", 0)
                h = d.get("h", 0)
                parts.append("{},{},{},{},{},{}".format(cls, conf, cx, cy, w, h))
            line = "DETS:" + ";".join(parts) + "\n"

        uart.write(line.encode('utf-8'))
        print("[串口→ESP32]", line.strip())
    except Exception as e:
        print("[串口] 发送失败:", e)

# ==================== 🎯 2. 自定义检测类 ====================
class CustomObjectDetectionApp(AIBase):
    def __init__(self, kmodel_path, labels, model_input_size, max_boxes_num, confidence_threshold=0.28, nms_threshold=0.25, rgb888p_size=[224,224], display_size=[1920,1080], debug_mode=0):
        super().__init__(kmodel_path, model_input_size, rgb888p_size, debug_mode)
        self.kmodel_path = kmodel_path
        self.labels = labels
        self.model_input_size = model_input_size
        self.confidence_threshold = confidence_threshold
        self.nms_threshold = nms_threshold
        self.max_boxes_num = max_boxes_num
        self.rgb888p_size = [ALIGN_UP(rgb888p_size[0], 16), rgb888p_size[1]]
        self.display_size = [ALIGN_UP(display_size[0], 16), display_size[1]]
        self.x_factor = float(self.rgb888p_size[0]) / self.model_input_size[0]
        self.y_factor = float(self.rgb888p_size[1]) / self.model_input_size[1]
        self.color_list = [(255, 220, 20, 60), (255, 119, 11, 32), (255, 0, 0, 142), (255, 0, 0, 230)]
        self.ai2d = Ai2d(debug_mode)
        self.ai2d.set_ai2d_dtype(nn.ai2d_format.NCHW_FMT, nn.ai2d_format.NCHW_FMT, np.uint8, np.uint8)

    def config_preprocess(self, input_image_size=None):
        ai2d_input_size = input_image_size if input_image_size else self.rgb888p_size
        self.ai2d.resize(nn.interp_method.tf_bilinear, nn.interp_mode.half_pixel)
        self.ai2d.build([1, 3, ai2d_input_size[1], ai2d_input_size[0]], [1, 3, self.model_input_size[1], self.model_input_size[0]])

    def postprocess(self, results):
        result = results[0].reshape((results[0].shape[0] * results[0].shape[1], results[0].shape[2]))
        output_data = result.transpose()
        boxes_ori, scores_ori = output_data[:, 0:4], output_data[:, 4:]
        confs_ori, inds_ori = np.max(scores_ori, axis=-1), np.argmax(scores_ori, axis=-1)
        boxes, scores, inds = [], [], []
        for i in range(len(boxes_ori)):
            if confs_ori[i] > self.confidence_threshold:
                scores.append(confs_ori[i]); inds.append(inds_ori[i])
                x, y, w, h = boxes_ori[i, 0], boxes_ori[i, 1], boxes_ori[i, 2], boxes_ori[i, 3]
                boxes.append([int((x-0.5*w)*self.x_factor), int((y-0.5*h)*self.y_factor), int((x+0.5*w)*self.x_factor), int((y+0.5*h)*self.y_factor)])
        if len(boxes) == 0: return []
        keep = self.nms(boxes, scores, self.nms_threshold)
        return [[boxes[i][0], boxes[i][1], boxes[i][2], boxes[i][3], scores[i], inds[i]] for i in keep][:self.max_boxes_num]

    def nms(self, boxes, scores, thresh):
        x1 = [float(b[0]) for b in boxes]; y1 = [float(b[1]) for b in boxes]
        x2 = [float(b[2]) for b in boxes]; y2 = [float(b[3]) for b in boxes]
        scores_list = [float(s) for s in scores]
        areas = [(x2[i]-x1[i]+1)*(y2[i]-y1[i]+1) for i in range(len(x1))]
        order = sorted(range(len(scores_list)), key=lambda k: scores_list[k], reverse=True)
        keep = []
        while len(order) > 0:
            i = order[0]; keep.append(i)
            new_order = []
            for j in order[1:]:
                xx1, yy1 = max(x1[i], x1[j]), max(y1[i], y1[j])
                xx2, yy2 = min(x2[i], x2[j]), min(y2[i], y2[j])
                w, h = max(0.0, xx2-xx1+1), max(0.0, yy2-yy1+1)
                if (w*h) / (areas[i]+areas[j]-(w*h)) < thresh: new_order.append(j)
            order = new_order
        return keep

    def draw_result(self, pl, dets):
        pl.osd_img.clear()
        for det in dets:
            x, y = det[0]*self.display_size[0]//self.rgb888p_size[0], det[1]*self.display_size[1]//self.rgb888p_size[1]
            w, h = (det[2]-det[0])*self.display_size[0]//self.rgb888p_size[0], (det[3]-det[1])*self.display_size[1]//self.rgb888p_size[1]
            pl.osd_img.draw_rectangle(x, y, w, h, color=self.color_list[int(det[5])%len(self.color_list)], thickness=4)
            cls_name = self.labels[int(det[5])] if int(det[5]) < len(self.labels) else "unknown"
            pl.osd_img.draw_string_advanced(x, y-35, 26, f" {cls_name} {round(det[4],2)}", color=(255, 255, 255, 255))

# ==================== 🚀 3. 主程序 ====================
if __name__ == "__main__":
    rgb888p_size, display_size = [320, 320], [800, 480]
    # 【更新】指向你的 14 类模型
    kmodel_path = "/sdcard/examples/kmodel/best.kmodel"
    # 【更新】14 类标签顺序
    labels = [
        'blind_track', 'curb', 'crosswalk', 'pole', 'ashcan',
        'reflective_cone', 'red_light', 'yellow_light', 'green_light', 'stop_sign',
        'person', 'vehicle', 'stairs', 'puddle'
    ]

    pl = PipeLine(rgb888p_size=rgb888p_size, display_size=display_size, display_mode='st7701')
    pl.create(Sensor(width=1920, height=1080))
    ob_det = CustomObjectDetectionApp(kmodel_path, labels=labels, model_input_size=[320, 320], max_boxes_num=15, confidence_threshold=0.15, rgb888p_size=rgb888p_size, display_size=display_size)
    ob_det.config_preprocess()

    frame_counter = 0
    while True:
        img = pl.get_frame()
        res = ob_det.run(img)
        ob_det.draw_result(pl, res)

        frame_counter += 1
        if frame_counter % 3 == 0 and res:
            upload_detections([{"x": int(d[0]), "y": int(d[1]), "w": int(d[2]-d[0]), "h": int(d[3]-d[1]), "label": LABEL_MAP.get(labels[int(d[5])], (labels[int(d[5])], "#00d4ff"))[0], "class": labels[int(d[5])], "confidence": round(float(d[4]), 2)} for d in res])

        pl.show_image()
        gc.collect()
