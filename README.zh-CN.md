# Dino Toy（ESP32-S3）

[English](README.md) | [简体中文](README.zh-CN.md)

这是一个基于 **ESP32-S3**（ESP-IDF 5.x）的语音互动恐龙玩具项目：

- 🎤 唤醒词 + 本地离线指令（控制 LED / 舵机 / 播放吼叫 MP3）
- 📺 ST7789 LCD 表情显示（可选）
- 🌐 Wi‑Fi 配网 + 设备端网页控制
- 💬 WebSocket 实时流式对话（低延迟）
- 🔄 OTA 固件升级

## 功能特性

### 语音交互
- **唤醒词**：`小鹿，小鹿`
- **本地离线命令词**（MultiNet）：
  - `开灯` / `关灯`
  - `前进` / `后退`（舵机到预设角度）
  - `神龙摆尾`（舵机摆动 + LED 闪烁 + 播放内置 `dinosaur-roar.mp3`）

### 云端对话
- **WebSocket 协议**：实时双向流式传输，边录音边上传边播放
- **HTTP Proxy**：传统方式，录音完成后上传

### 显示系统 (可选)
- **ST7789 LCD**：240x240 彩色显示屏
- **表情动画**：neutral、happy、sad、thinking、listening、speaking、error
- **状态显示**：待机、聆听中、思考中、说话中等

### LED 状态指示
- **空闲**：呼吸灯
- **聆听**：呼吸灯（较亮）
- **处理中**：中速闪烁
- **播放中**：常亮
- **错误**：快速闪烁

### 其他功能
- **网页控制**：打开 `http://<设备IP>/` 可触发动作、查看状态、输入文本 TTS
- **Wi‑Fi 配网**：未保存 Wi‑Fi 时启动热点 `ESP32-Setup`
- **OTA 升级**：支持 HTTP 固件升级

## 硬件准备

### 必需硬件
- ESP32-S3 开发板（**建议 16MB Flash**）
- I2S 麦克风（如 **INMP441**）
- I2S 功放（如 **MAX98357A**）+ 喇叭
- 舵机（如 **SG90**）——建议外接 5V 供电
- LED

### 可选硬件
- ST7789 LCD 显示屏（240x240，SPI 接口）

## 引脚配置

| 功能 | GPIO | 说明 |
|---|---:|---|
| LED | 18 | PWM 呼吸灯 |
| 舵机 | 7 | 0–180° |
| I2S 麦克风 BCK | 41 | INMP441 SCK |
| I2S 麦克风 WS | 42 | INMP441 WS/LRCL |
| I2S 麦克风 DIN | 2 | INMP441 SD |
| I2S 功放 BCK | 15 | MAX98357 BCLK |
| I2S 功放 WS | 16 | MAX98357 LRC |
| I2S 功放 DOUT | 17 | MAX98357 DIN |

### ST7789 显示屏引脚（可选）

| 功能 | GPIO | 说明 |
|---|---:|---|
| MOSI | - | 需配置 |
| SCLK | - | 需配置 |
| CS | - | 需配置 |
| DC | - | 需配置 |
| RST | - | 需配置 |
| BL | - | 背光控制 |

## 编译与烧录

### 前置条件
- ESP-IDF **v5.0+**
- 开发板 USB 串口连接正常

### 编译

```bash
idf.py set-target esp32s3
idf.py build
```

### 烧录

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

> **注意**：至少执行一次 `idf.py flash`（不要只用 `idf.py app-flash`），因为语音模型会被写入 `model` 分区。

## 项目结构

```
dino_toy/
├── main/                   # 入口（app_main）
├── components/BSP/
│   ├── STATE_MACHINE/      # 设备状态机
│   ├── LED/                # LED 控制（PWM 呼吸灯）
│   ├── SERVO/              # 舵机控制
│   ├── WAKE_WORD/          # 唤醒词识别
│   ├── VOICE_CONTROL/      # 语音命令执行
│   ├── VOICE_DIALOG/       # 语音对话管理
│   ├── WEBSOCKET_CHAT/     # WebSocket 实时对话
│   ├── CLOUD_CHAT/         # HTTP 云端对话
│   ├── CLOUD_TTS/          # 云端 TTS
│   ├── DISPLAY/            # ST7789 显示屏
│   ├── OTA/                # 固件升级
│   ├── WIFI/               # WiFi 管理
│   └── MP3_PLAYER/         # MP3 播放
├── server/qwen_tts_proxy/  # 云端代理服务
└── partitions-16MB.csv     # 16MB 分区表
```

## 状态机

```
┌─────────────┐
│   Unknown   │
└──────┬──────┘
       │
       ▼
┌─────────────┐     ┌─────────────────┐
│  Starting   │────▶│ WifiConfiguring │
└──────┬──────┘     └────────┬────────┘
       │                     │
       ▼                     ▼
┌─────────────┐◀────────────────────────┐
│    Idle     │                         │
└──────┬──────┘                         │
       │                                │
       ▼                                │
┌─────────────┐     ┌─────────────┐     │
│  Listening  │────▶│ Processing  │─────┤
└──────┬──────┘     └──────┬──────┘     │
       │                   │            │
       ▼                   ▼            │
       └─────────┬─────────┘            │
                 ▼                      │
          ┌─────────────┐               │
          │  Speaking   │───────────────┘
          └─────────────┘
```

## 云端服务配置

### WebSocket 对话（推荐）

WebSocket 协议支持实时双向流式传输，延迟更低：

1. 在 `menuconfig` 中配置 WebSocket URL
2. 服务端需支持 WebSocket 协议

### HTTP Proxy 对话

详见 `server/qwen_tts_proxy/README.md`。

```bash
cd server/qwen_tts_proxy
pip install -r requirements.txt
export DASHSCOPE_API_KEY="你的 Key"
uvicorn app:app --host 0.0.0.0 --port 8000
```

## 常见问题

- **串口提示模型加载失败**：确认使用了 `partitions-16MB.csv`，并执行过 `idf.py flash`
- **打不开网页**：先连热点 `ESP32-Setup`，访问 `http://192.168.4.1/`
- **舵机动作导致重启**：建议外接 5V，注意共地，并加滤波电容

## 许可证

MIT，见 `LICENSE`。
