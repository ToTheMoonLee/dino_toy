# Dino Toy (ESP32-S3)

[English](README.md) | [简体中文](README.zh-CN.md)

A voice-controlled dinosaur toy built on **ESP32-S3** (ESP-IDF 5.x):

- Wake word + offline command words (LED / servo / roar MP3)
- Wi‑Fi provisioning + on-device web control page
- Optional cloud dialog & TTS via a LAN proxy (Qwen/DashScope)

## Features

- **Wake word**: `小鹿，小鹿`
- **Offline commands** (MultiNet):
  - `开灯` / `关灯`
  - `前进` / `后退` (servo to preset angles)
  - `神龙摆尾` (servo swing + LED blink + play embedded `dinosaur-roar.mp3`)
- **Web control**: open `http://<device-ip>/` to trigger commands, view status, and send TTS text
- **Wi‑Fi provisioning**: if no Wi‑Fi is saved (or STA fails), device starts SoftAP `ESP32-Setup` and serves a setup page
- **Cloud dialog (optional)**: after wake, stream mic audio to a server and play back assistant speech (`/chat`) or low-latency PCM streaming (`/chat_pcm`)

## Hardware

- ESP32-S3 dev board (**16MB flash recommended**)  
  This project ships a 16MB partition table: `partitions-16MB.csv` (includes a `model` partition for ESP-SR models).
- I2S microphone (e.g. **INMP441**)
- I2S amplifier (e.g. **MAX98357A**) + speaker
- Servo (e.g. **SG90**) — use an external 5V supply if possible
- LED (or WS2812/other LED connected to a GPIO if you modify the driver)

## Pin Mapping (defaults)

Pins are currently configured in `main/main.cpp`.

| Function | GPIO | Notes |
|---|---:|---|
| LED | 18 | on/off + blink |
| Servo | 7 | 0–180° |
| I2S MIC BCK | 41 | INMP441 SCK |
| I2S MIC WS | 42 | INMP441 WS/LRCL |
| I2S MIC DIN | 2 | INMP441 SD |
| I2S AMP BCK | 15 | MAX98357 BCLK |
| I2S AMP WS | 16 | MAX98357 LRC |
| I2S AMP DOUT | 17 | MAX98357 DIN |

Notes:

- The mic input is configured as **left channel**; for INMP441, connect **L/R to GND** to output left channel.
- Servo + amp can draw current spikes; **share GND** and consider adding a capacitor near the servo/amp supply.

## Build & Flash (ESP-IDF)

Prerequisites:

- ESP-IDF **v5.0+**
- USB serial connection to the ESP32-S3 board

Optional (VS Code):

- This repo includes a `.devcontainer/` based on `espressif/idf` for a ready-to-use ESP-IDF environment.

Build:

```bash
idf.py set-target esp32s3
idf.py menuconfig
idf.py build
```

Flash + monitor:

```bash
idf.py -p /dev/ttyUSB0 flash monitor
```

Important:

- Use **`idf.py flash`** (not only `idf.py app-flash`) at least once, because the speech models are flashed into the `model` partition.

### Menuconfig checklist

- `Serial flasher config`:
  - `Flash size` → `16MB` (when using the provided 16MB partition table)
- `Partition Table`:
  - `Partition Table` → `Custom partition table CSV`
  - `Custom partition CSV file` → `partitions-16MB.csv`
- `Component config → ESP Speech Recognition`:
  - `model data path` → `Read model data from flash`
  - `Load Multiple Wake Words (WakeNet9)` → enable **one** wake word model (recommended: `小鹿小鹿 (wn9_xiaoluxiaolu_tts2)`)
  - `Chinese Speech Commands Model` → `general chinese recognition (mn7_cn)`
- `Component config → Audio playback`:
  - `Enable mp3 decoding.` → **ON** (required for the roar SFX)
  - `Enable wav file playback` → **ON** (used by cloud TTS / assistant playback)
- `Component config → Cloud Voice` (optional):
  - `Cloud TTS proxy URL` → `http://<your-pc-ip>:8000/tts`
  - `Cloud Chat proxy URL` → `http://<your-pc-ip>:8000/chat`
  - `Cloud Chat PCM stream URL (low latency)` → `http://<your-pc-ip>:8000/chat_pcm`
  - Tuning (usually keep defaults):
    - `Dialog session timeout (ms)` (e.g. `45000`)
    - `End-of-utterance silence (ms)` (e.g. `450`)
    - `Dialog speech energy gate (mean abs)` (e.g. `120–300`, higher = less false triggers)
    - `Ignore dialog audio after local command (ms)` (e.g. `800`)
    - `Max utterance length (ms)` (e.g. `8000`)

## Wi‑Fi Provisioning & Web Control

- If the device has no saved Wi‑Fi (or fails to connect), it starts a SoftAP:
  - SSID: `ESP32-Setup`
  - Password: empty (open network by default)
- Connect your phone/PC to `ESP32-Setup`, then open:
  - Control page: `http://192.168.4.1/`
  - Wi‑Fi setup: `http://192.168.4.1/wifi`
- After connecting to your router, check the serial log for the STA IP and open:
  - `http://<sta-ip>/`

## Voice Commands

1. Say the wake word: `小鹿，小鹿`
2. Then say one of the offline commands within a few seconds:
   - `开灯` / `关灯` / `前进` / `后退` / `神龙摆尾`
3. (Optional) Cloud dialog:
   - If a Cloud Chat URL is configured, after waking up you can speak freely and the device will enter a multi-turn dialog session until timeout.

## Cloud Proxy (Qwen/DashScope)

This repo includes a small FastAPI proxy server at `server/qwen_tts_proxy` to keep API keys off the ESP32.
For more options (models, realtime TTS, etc.), see `server/qwen_tts_proxy/README.md`.

Endpoints:

- `POST /tts` (text → WAV)
- `POST /chat` (WAV → ASR → LLM → TTS → WAV)
- `POST /chat_pcm` (WAV → streaming ASR/LLM/TTS → **raw PCM stream**, 16kHz/mono/s16le)

Run it:

```bash
cd server/qwen_tts_proxy
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt

export DASHSCOPE_API_KEY="your_key"
uvicorn app:app --host 0.0.0.0 --port 8000
```

Then set the URLs via `idf.py menuconfig` → `Cloud Voice`.

## Web API (on-device)

- `GET /api/status` → JSON status
- `GET /api/cmd?id=0..4` → trigger a command
- `POST /api/tts` (`Content-Type: text/plain; charset=utf-8`) → triggers TTS playback (requires `CONFIG_CLOUD_TTS_PROXY_URL`)
- `POST /api/wifi/save` (`application/x-www-form-urlencoded`) → save SSID/password and connect

## Project Layout

- `main/` — app entry (`app_main`)
- `components/BSP/` — LED / servo / wake word / dialog / Wi‑Fi web manager
- `server/qwen_tts_proxy/` — optional LAN proxy for cloud ASR/LLM/TTS
- `partitions-16MB.csv` — 16MB partition table (includes `model` partition for ESP-SR models)

## Troubleshooting

- `模型加载失败,请检查 model 分区` / wake-word model not found:
  - Ensure you are using `partitions-16MB.csv` and ran `idf.py flash` (not only `app-flash`).
- Web page not reachable:
  - Connect to `ESP32-Setup` and try `http://192.168.4.1/`.
  - If STA connected, check the serial log for the assigned IP.
- Servo causes resets/noise:
  - Use an external 5V supply, share GND with ESP32, and add decoupling.

## License

MIT. See `LICENSE`.
