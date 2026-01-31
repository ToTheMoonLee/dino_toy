# Dino Toy (ESP32-S3)

[English](README.md) | [简体中文](README.zh-CN.md)

A voice-interactive dinosaur toy project based on **ESP32-S3** (ESP-IDF 5.x):

- 🎤 Wake word + offline voice commands (LED / Servo / MP3 playback)
- 📺 ST7789 LCD emotion display (optional)
- 🌐 Wi-Fi provisioning + Web control
- 💬 WebSocket real-time streaming dialogue (low latency)
- 🔄 OTA firmware upgrade

## Features

### Voice Interaction
- **Wake Word**: `小鹿，小鹿` (Xiaolu, Xiaolu)
- **Offline Commands** (MultiNet):
  - `开灯` / `关灯` (Light on/off)
  - `前进` / `后退` (Servo forward/backward)
  - `神龙摆尾` (Dragon tail swing - servo + LED flash + roar MP3)

### Cloud Dialogue
- **WebSocket Protocol**: Real-time bidirectional streaming
- **HTTP Proxy**: Traditional request-response mode

### Display System (Optional)
- **ST7789 LCD**: 240x240 color display
- **Emotion Animations**: neutral, happy, sad, thinking, listening, speaking, error

### LED Status Indicators
- **Idle**: Breathing effect
- **Listening**: Breathing (brighter)
- **Processing**: Medium blink
- **Speaking**: Solid on
- **Error**: Fast blink

### Other Features
- **Web Control**: Access `http://<device-ip>/` for actions and status
- **Wi-Fi Provisioning**: `ESP32-Setup` hotspot when not configured
- **OTA Upgrade**: HTTP firmware updates

## Hardware Requirements

### Required
- ESP32-S3 board (**16MB Flash recommended**)
- I2S Microphone (e.g., INMP441)
- I2S Amplifier (e.g., MAX98357A) + Speaker
- Servo (e.g., SG90)
- LED

### Optional
- ST7789 LCD Display (240x240, SPI)

## Pin Configuration

| Function | GPIO | Description |
|---|---:|---|
| LED | 18 | PWM breathing |
| Servo | 7 | 0–180° |
| I2S Mic BCK | 41 | INMP441 SCK |
| I2S Mic WS | 42 | INMP441 WS |
| I2S Mic DIN | 2 | INMP441 SD |
| I2S Amp BCK | 15 | MAX98357 BCLK |
| I2S Amp WS | 16 | MAX98357 LRC |
| I2S Amp DOUT | 17 | MAX98357 DIN |

## Build & Flash

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash monitor
```

## Project Structure

```
dino_toy/
├── main/                   # Entry point
├── components/BSP/
│   ├── STATE_MACHINE/      # Device state machine
│   ├── LED/                # LED control (PWM)
│   ├── SERVO/              # Servo control
│   ├── WAKE_WORD/          # Wake word detection
│   ├── VOICE_CONTROL/      # Voice command execution
│   ├── VOICE_DIALOG/       # Voice dialogue management
│   ├── WEBSOCKET_CHAT/     # WebSocket real-time chat
│   ├── CLOUD_CHAT/         # HTTP cloud chat
│   ├── DISPLAY/            # ST7789 display
│   ├── OTA/                # Firmware upgrade
│   └── WIFI/               # WiFi management
├── server/qwen_tts_proxy/  # Cloud proxy service
└── partitions-16MB.csv     # 16MB partition table
```

## License

MIT, see `LICENSE`.
