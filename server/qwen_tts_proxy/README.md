# Qwen Voice Proxy (for ESP32)

这个小服务把 **DashScope/Qwen ASR/LLM/TTS** 的鉴权与返回解析放到 PC/服务器上做：

- `/tts`：ESP32 只需要 `POST` 文本到 `/tts`，服务端调用 Qwen TTS 并返回 `audio/wav`
- `/chat`：ESP32 `POST audio/wav` 到 `/chat`，服务端做 **ASR → LLM → TTS**，返回 `audio/wav`
- `/chat_pcm`：ESP32 `POST audio/wav` 到 `/chat_pcm`，服务端做 **ASR → LLM(流式) → 实时TTS**，以 **PCM 流**返回（更低延迟，边生成边播）

## 运行

1) 安装依赖

```bash
cd server/qwen_tts_proxy
python -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
```

2) 配置 Key 并启动

```bash
export DASHSCOPE_API_KEY="你的 DashScope Key"
# 让对话更快、更自然（推荐）
export QWEN_LLM_MODEL="qwen-turbo"              # 可选：更快（如果你的账号可用）
export QWEN_MAX_TOKENS="160"                   # 控制回复长度
export QWEN_TEMPERATURE="0.3"                  # 更稳定
export QWEN_TTS_SAMPLE_RATE="16000"            # 设备侧更省流量/更不容易超 1MB
export QWEN_TTS_VOICE="Cherry"                 # 可选
# realtime TTS（用于 /chat_pcm）
export QWEN_TTS_REALTIME_MODEL="qwen3-tts-flash-realtime"
# 如遇到 ws 连接问题可手动指定（国内/国际）
# export DASHSCOPE_REALTIME_WS_URL="wss://dashscope.aliyuncs.com/api-ws/v1/realtime"
# export DASHSCOPE_REALTIME_WS_URL="wss://dashscope-intl.aliyuncs.com/api-ws/v1/realtime"
uvicorn app:app --host 0.0.0.0 --port 8000
```

3) 测试 TTS

```bash
curl -X POST http://127.0.0.1:8000/tts -H 'Content-Type: text/plain; charset=utf-8' --data '你好，我是ESP32' --output out.wav
```

4) 测试 Chat（上传一段 wav）

```bash
curl -X POST http://127.0.0.1:8000/chat -H 'Content-Type: audio/wav' --data-binary @out.wav --output reply.wav
```

5) 测试 Chat PCM（更低延迟，返回原始 PCM，需要 ffmpeg 才能直接播放）

```bash
curl -X POST http://127.0.0.1:8000/chat_pcm -H 'Content-Type: audio/wav' --data-binary @out.wav --output reply.pcm
# 例：转成 wav（按 16k/mono/16bit）
ffmpeg -f s16le -ar 16000 -ac 1 -i reply.pcm reply.wav
```

## 固件侧配置

在 `idf.py menuconfig` 里设置：

- `Cloud Voice -> Cloud TTS proxy URL`
  - 例如 `http://你的电脑IP:8000/tts`

- `Cloud Voice -> Cloud Chat proxy URL`
  - 例如 `http://你的电脑IP:8000/chat`

（可选，更低延迟）如果你使用 `/chat_pcm`：

- `Cloud Voice -> Cloud Chat PCM stream URL`
  - 例如 `http://你的电脑IP:8000/chat_pcm`

然后刷机：

- 打开 ESP32 控制页 `/`：可用 `TTS` 输入框做文本朗读
- 说唤醒词后直接讲话：进入对话模式，多轮对话直到超时退出
