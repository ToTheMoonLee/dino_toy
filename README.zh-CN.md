# Dino Toy（ESP32-S3）

[English](README.md) | [简体中文](README.zh-CN.md)

这是一个基于 **ESP32-S3**（ESP-IDF 5.x）的语音互动恐龙玩具项目：

- 唤醒词 + 本地离线指令（控制 LED / 舵机 / 播放吼叫 MP3）
- Wi‑Fi 配网 + 设备端网页控制
- 可选：通过局域网 Proxy 接入云端对话与 TTS（Qwen/DashScope）

## 功能特性

- **唤醒词**：`小鹿，小鹿`
- **本地离线命令词**（MultiNet）：
  - `开灯` / `关灯`
  - `前进` / `后退`（舵机到预设角度）
  - `神龙摆尾`（舵机摆动 + LED 闪烁 + 播放内置 `dinosaur-roar.mp3`）
- **网页控制**：打开 `http://<设备IP>/` 可触发动作、查看状态、输入文本 TTS
- **Wi‑Fi 配网**：未保存 Wi‑Fi（或 STA 连接失败）时启动热点 `ESP32-Setup` 并提供配网页面
- **云端对话（可选）**：唤醒后将麦克风音频上传到服务器并播放回复（`/chat` 返回 WAV，或 `/chat_pcm` 低延迟 PCM 流）

## 硬件准备

- ESP32-S3 开发板（**建议 16MB Flash**）  
  项目提供 16MB 分区表：`partitions-16MB.csv`（包含 ESP-SR 模型使用的 `model` 分区）。
- I2S 麦克风（如 **INMP441**）
- I2S 功放（如 **MAX98357A**）+ 喇叭
- 舵机（如 **SG90**）——建议外接 5V 供电
- LED（或你自行改驱动接入 WS2812 等）

## 引脚默认配置

引脚目前在 `main/main.cpp` 里配置。

| 功能 | GPIO | 说明 |
|---|---:|---|
| LED | 18 | 开关 + 闪烁 |
| 舵机 | 7 | 0–180° |
| I2S 麦克风 BCK | 41 | INMP441 SCK |
| I2S 麦克风 WS | 42 | INMP441 WS/LRCL |
| I2S 麦克风 DIN | 2 | INMP441 SD |
| I2S 功放 BCK | 15 | MAX98357 BCLK |
| I2S 功放 WS | 16 | MAX98357 LRC |
| I2S 功放 DOUT | 17 | MAX98357 DIN |

注意：

- 麦克风输入配置为 **左声道**；INMP441 请把 **L/R 接 GND** 输出左声道数据。
- 舵机/功放瞬时电流较大，务必 **共地**，并考虑在舵机/功放电源附近加电容。

## 编译与烧录（ESP-IDF）

前置条件：

- ESP-IDF **v5.0+**
- 开发板 USB 串口连接正常

可选（VS Code）：

- 本仓库包含 `.devcontainer/`（基于 `espressif/idf`），可一键获得可用的 ESP-IDF 开发环境。

编译：

```bash
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

烧录并监视串口：

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

重要说明：

- 至少执行一次 **`idf.py flash`**（不要只用 `idf.py app-flash`），因为语音模型会被写入 `model` 分区。

### Menuconfig 必配项

- `Serial flasher config`：
  - `Flash size` → `16MB`（使用 16MB 分区表时必须匹配）
- `Partition Table`：
  - `Partition Table` → `Custom partition table CSV`
  - `Custom partition CSV file` → `partitions-16MB.csv`
- `Component config → ESP Speech Recognition`：
  - `model data path` → `Read model data from flash`
  - `Load Multiple Wake Words (WakeNet9)` → 只勾选 **一个** 唤醒词模型（推荐：`小鹿小鹿 (wn9_xiaoluxiaolu_tts2)`）
  - `Chinese Speech Commands Model` → `general chinese recognition (mn7_cn)`
- `Component config → Audio playback`：
  - `Enable mp3 decoding.` → **开启**（“吼叫”音效需要）
  - `Enable wav file playback` → **开启**（云端 TTS/对话回放需要）
- `Component config → Cloud Voice`（可选）：
  - `Cloud TTS proxy URL` → `http://<你的电脑IP>:8000/tts`
  - `Cloud Chat proxy URL` → `http://<你的电脑IP>:8000/chat`
  - `Cloud Chat PCM stream URL (low latency)` → `http://<你的电脑IP>:8000/chat_pcm`
  - 参数调优（一般保持默认即可）：
    - `Dialog session timeout (ms)`（例如 `45000`）
    - `End-of-utterance silence (ms)`（例如 `450`）
    - `Dialog speech energy gate (mean abs)`（例如 `120–300`，越大越不容易误触发/误上传）
    - `Ignore dialog audio after local command (ms)`（例如 `800`）
    - `Max utterance length (ms)`（例如 `8000`）

## Wi‑Fi 配网与网页控制

- 未保存 Wi‑Fi（或连接失败）时，设备会启动热点：
  - SSID：`ESP32-Setup`
  - 密码：空（默认开放热点）
- 手机/电脑连接 `ESP32-Setup` 后打开：
  - 控制页：`http://192.168.4.1/`
  - 配网页：`http://192.168.4.1/wifi`
- 连上路由器后，串口日志会打印 STA IP，用浏览器打开：
  - `http://<sta-ip>/`

## 语音使用方式

1. 说唤醒词：`小鹿，小鹿`
2. 然后在几秒内说本地命令词之一：
   - `开灯` / `关灯` / `前进` / `后退` / `神龙摆尾`
3. （可选）云端对话：
   - 如果配置了 Cloud Chat URL，唤醒后可直接自由说话，设备会进入多轮对话，直到超时退出。

## 云端 Proxy（Qwen/DashScope）

仓库内置了一个 FastAPI 小服务：`server/qwen_tts_proxy`，用于把 Key 放在电脑/服务器侧，ESP32 只访问局域网 HTTP 接口。
更多可选项（模型、realtime TTS 等）请参考：`server/qwen_tts_proxy/README.md`。

接口说明：

- `POST /tts`（文本 → WAV）
- `POST /chat`（WAV → ASR → LLM → TTS → WAV）
- `POST /chat_pcm`（WAV → 流式 ASR/LLM/TTS → **PCM 流**，16kHz/mono/s16le）

运行方式：

```bash
cd server/qwen_tts_proxy
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt

export DASHSCOPE_API_KEY="你的 Key"
uvicorn app:app --host 0.0.0.0 --port 8000
```

然后在 `idf.py menuconfig` → `Cloud Voice` 里填入对应 URL。

## 设备端 Web API

- `GET /api/status` → JSON 状态
- `GET /api/cmd?id=0..4` → 触发命令
- `POST /api/tts`（`Content-Type: text/plain; charset=utf-8`）→ 触发 TTS 播放（需要配置 `CONFIG_CLOUD_TTS_PROXY_URL`）
- `POST /api/wifi/save`（`application/x-www-form-urlencoded`）→ 保存 Wi‑Fi 并连接

## 目录结构

- `main/` — 入口（`app_main`）
- `components/BSP/` — LED / 舵机 / 唤醒词 / 对话 / Wi‑Fi Web 管理
- `server/qwen_tts_proxy/` — 可选：云端 ASR/LLM/TTS 局域网代理
- `partitions-16MB.csv` — 16MB 分区表（包含 ESP-SR 模型的 `model` 分区）

## 常见问题

- 串口提示 `模型加载失败,请检查 model 分区` / 找不到唤醒词模型：
  - 确认使用了 `partitions-16MB.csv`，并且执行过 `idf.py flash`（不要只烧 app）。
- 打不开网页：
  - 先连热点 `ESP32-Setup`，访问 `http://192.168.4.1/`。
  - 如果 STA 已连接，查看串口日志里的 IP。
- 舵机动作导致重启/噪声：
  - 建议外接 5V，注意共地，并加滤波/去耦。

## 许可证

MIT，见 `LICENSE`。
