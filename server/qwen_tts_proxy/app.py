import base64
import audioop
import io
import json
import os
import queue
import threading
import wave
from typing import Optional

import requests
from fastapi import FastAPI, HTTPException, Request, Response, WebSocket, WebSocketDisconnect
from fastapi.responses import StreamingResponse
import asyncio
import uuid


DEFAULT_DASHSCOPE_TTS_URL = "https://dashscope.aliyuncs.com/api/v1/services/aigc/multimodal-generation/generation"
DEFAULT_DASHSCOPE_COMPAT_BASE = "https://dashscope.aliyuncs.com/compatible-mode/v1"
DEFAULT_DASHSCOPE_REALTIME_WS_URL = "wss://dashscope.aliyuncs.com/api-ws/v1/realtime"

_DASHSCOPE_IMPORT_ERROR: Optional[str] = None
try:
    import dashscope
    from dashscope.audio.qwen_tts_realtime import AudioFormat, QwenTtsRealtime, QwenTtsRealtimeCallback
except Exception as e:  # pragma: no cover
    _DASHSCOPE_IMPORT_ERROR = f"{type(e).__name__}: {e}"
    dashscope = None
    AudioFormat = None
    QwenTtsRealtime = None
    QwenTtsRealtimeCallback = object


def _env(name: str) -> str:
    v = os.getenv(name, "").strip()
    if not v:
        raise RuntimeError(f"Missing env var: {name}")
    return v


def _dashscope_compat_base() -> str:
    return os.getenv("DASHSCOPE_COMPAT_BASE_URL", DEFAULT_DASHSCOPE_COMPAT_BASE).rstrip("/")


def _dashscope_tts_url() -> str:
    return os.getenv("DASHSCOPE_TTS_URL", DEFAULT_DASHSCOPE_TTS_URL)

def _dashscope_realtime_ws_url() -> str:
    return os.getenv("DASHSCOPE_REALTIME_WS_URL", DEFAULT_DASHSCOPE_REALTIME_WS_URL).strip()


def _pcm_to_wav(pcm_bytes: bytes, sample_rate: int) -> bytes:
    bio = io.BytesIO()
    with wave.open(bio, "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)  # 16-bit PCM
        wf.setframerate(sample_rate)
        wf.writeframes(pcm_bytes)
    return bio.getvalue()


def _normalize_wav(wav_bytes: bytes, target_sr: int, target_channels: int = 1) -> bytes:
    """
    Normalize WAV for embedded playback:
    - downmix to mono (default)
    - resample to target sample rate (default via env)
    """
    try:
        with wave.open(io.BytesIO(wav_bytes), "rb") as wf:
            nch = wf.getnchannels()
            sampwidth = wf.getsampwidth()
            sr = wf.getframerate()
            frames = wf.readframes(wf.getnframes())
    except Exception:
        return wav_bytes

    if sampwidth != 2:
        return wav_bytes

    # downmix if needed
    if target_channels == 1 and nch == 2:
        frames = audioop.tomono(frames, 2, 0.5, 0.5)
        nch = 1

    # resample if needed
    if target_sr and sr and target_sr != sr:
        frames, _ = audioop.ratecv(frames, 2, nch, sr, target_sr, None)
        sr = target_sr

    # re-pack
    out = io.BytesIO()
    with wave.open(out, "wb") as wf:
        wf.setnchannels(nch)
        wf.setsampwidth(2)
        wf.setframerate(sr)
        wf.writeframes(frames)
    return out.getvalue()

def _audio_format_for_pcm(sample_rate: int) -> tuple[object, int]:
    """
    Map sample rate to dashscope AudioFormat enum.
    Falls back to 24000Hz if a specific enum value is not available.
    """
    if AudioFormat is None:
        raise RuntimeError("dashscope is not installed")

    sr = int(sample_rate or 24000)
    name = f"PCM_{sr}HZ_MONO_16BIT"
    fmt = getattr(AudioFormat, name, None)
    if fmt is not None:
        return fmt, sr

    fallback = getattr(AudioFormat, "PCM_24000HZ_MONO_16BIT", None)
    if fallback is None:
        raise RuntimeError(f"Unsupported AudioFormat (need {name})")
    return fallback, 24000


class _RealtimeTtsCallback(QwenTtsRealtimeCallback):  # type: ignore[misc]
    def __init__(self, out_q: "queue.Queue[Optional[bytes]]", stop_evt: threading.Event):
        self._q = out_q
        self._stop_evt = stop_evt
        self.error: Optional[Exception] = None

    def on_event(self, response):  # noqa: ANN001
        # Expected event types (from docs):
        # - response.audio.delta: {"delta": "<base64 pcm>"}
        # - session.finished
        if self._stop_evt.is_set():
            return
        try:
            t = (response or {}).get("type")
            if t == "response.audio.delta":
                b64 = (response or {}).get("delta") or ""
                if b64:
                    self._q.put(base64.b64decode(b64), timeout=5)
            elif t == "session.finished":
                self._stop_evt.set()
        except Exception as e:  # pragma: no cover
            self.error = e
            self._stop_evt.set()


def _qwen_tts_realtime_pcm_stream(
    api_key: str,
    text: str,
    voice: str,
    model: str,
    target_sample_rate: int,
) -> tuple[int, "queue.Queue[Optional[bytes]]", threading.Event, "_RealtimeTtsCallback"]:
    """
    Start a realtime TTS session in a background thread and return a queue that
    yields PCM (16-bit mono) chunks.

    Returns (sample_rate, queue, stop_event, callback).
    """
    if dashscope is None or QwenTtsRealtime is None:
        hint = _DASHSCOPE_IMPORT_ERROR or "dashscope is not installed"
        raise RuntimeError(
            "dashscope import failed: "
            + hint
            + ". Please install/upgrade it in the SAME venv that runs uvicorn: "
            + "`python -m pip install -r requirements.txt`"
        )

    target_sr = int(target_sample_rate or 16000)

    # Some realtime models only support fixed output rates (often 24k).
    # We request the closest enum we can; then optionally resample in the HTTP stream.
    requested_fmt, requested_sr = _audio_format_for_pcm(target_sr)

    out_q: "queue.Queue[Optional[bytes]]" = queue.Queue(maxsize=256)
    stop_evt = threading.Event()
    cb = _RealtimeTtsCallback(out_q, stop_evt)

    ws_url = _dashscope_realtime_ws_url()

    def _run():
        client = None
        try:
            dashscope.api_key = api_key
            client = QwenTtsRealtime(model=model, callback=cb, url=ws_url)
            client.connect()
            client.update_session(
                voice=voice,
                response_format=requested_fmt,
                mode="server_commit",
            )
            client.append_text(text)
            client.finish()
            # Block until finished or error/stop.
            stop_evt.wait(timeout=120)
        except Exception as e:  # pragma: no cover
            cb.error = e
            stop_evt.set()
        finally:
            try:
                if client is not None and hasattr(client, "close"):
                    client.close()
            except Exception:
                pass
            # Sentinel to end the HTTP stream
            try:
                out_q.put(None, timeout=1)
            except Exception:
                pass

    threading.Thread(target=_run, daemon=True).start()

    return requested_sr, out_q, stop_evt, cb


def _qwen_chat_to_realtime_tts_pcm_stream(
    api_key: str,
    msgs: list[dict],
    llm_model: str,
    voice: str,
    tts_model: str,
    target_sample_rate: int,
) -> tuple[int, "queue.Queue[Optional[bytes]]", threading.Event, "_RealtimeTtsCallback"]:
    """
    Stream LLM output into realtime TTS so audio can start before the full text is ready.
    Returns (tts_output_sample_rate, queue, stop_event, callback).
    """
    if dashscope is None or QwenTtsRealtime is None:
        hint = _DASHSCOPE_IMPORT_ERROR or "dashscope is not installed"
        raise RuntimeError(
            "dashscope import failed: "
            + hint
            + ". Please install/upgrade it in the SAME venv that runs uvicorn: "
            + "`python -m pip install -r requirements.txt`"
        )

    target_sr = int(target_sample_rate or 16000)
    requested_fmt, requested_sr = _audio_format_for_pcm(target_sr)

    out_q: "queue.Queue[Optional[bytes]]" = queue.Queue(maxsize=256)
    stop_evt = threading.Event()
    cb = _RealtimeTtsCallback(out_q, stop_evt)

    ws_url = _dashscope_realtime_ws_url()
    keep_turns = int(os.getenv("QWEN_MAX_TURNS", "12"))

    def _run():
        client = None
        try:
            dashscope.api_key = api_key
            client = QwenTtsRealtime(model=tts_model, callback=cb, url=ws_url)
            client.connect()
            client.update_session(
                voice=voice,
                response_format=requested_fmt,
                mode="server_commit",
            )

            assistant_text_parts: list[str] = []
            pending = ""
            sent_any = False

            # Stream LLM output and feed to TTS incrementally.
            try:
                for delta in _qwen_chat_stream(api_key=api_key, model=llm_model, messages=msgs):
                    assistant_text_parts.append(delta)
                    pending += delta

                    # Flush strategy: ensure early first audio, then flush on punctuation/length.
                    if (not sent_any and len(pending) >= 8) or len(pending) >= 24 or pending.endswith(
                        ("。", "！", "？", "!", "?", "\n")
                    ):
                        client.append_text(pending)
                        pending = ""
                        sent_any = True
            except Exception:
                # Fallback to non-streaming if provider doesn't support stream.
                full = _qwen_chat(api_key=api_key, model=llm_model, messages=msgs)
                assistant_text_parts = [full]
                pending = full

            if pending:
                client.append_text(pending)

            full_text = "".join(assistant_text_parts).strip()
            if not full_text:
                full_text = "我想了一下，但不知道怎么回答。"
                client.append_text(full_text)

            msgs.append({"role": "assistant", "content": full_text})
            _trim_session(msgs, keep=keep_turns)

            client.finish()
            stop_evt.wait(timeout=120)
        except Exception as e:  # pragma: no cover
            cb.error = e
            stop_evt.set()
        finally:
            try:
                if client is not None and hasattr(client, "close"):
                    client.close()
            except Exception:
                pass
            try:
                out_q.put(None, timeout=1)
            except Exception:
                pass

    threading.Thread(target=_run, daemon=True).start()

    return requested_sr, out_q, stop_evt, cb


app = FastAPI(title="Qwen TTS Proxy", version="0.1.0")

# In-memory session store: device_id -> messages
_SESSIONS: dict[str, list[dict]] = {}

@app.on_event("startup")
def _startup_log() -> None:
    if dashscope is None:
        print(f"[startup] dashscope unavailable: {_DASHSCOPE_IMPORT_ERROR or 'unknown error'}")
    else:
        ver = getattr(dashscope, "__version__", "unknown")
        print(f"[startup] dashscope ok: {ver}")


def _session(device_id: str) -> list[dict]:
    device_id = (device_id or "default").strip() or "default"
    msgs = _SESSIONS.get(device_id)
    if msgs is None:
        system_prompt = os.getenv(
            "QWEN_SYSTEM_PROMPT",
            "你是一个简洁、友好的语音助手。用口语化的中文回答。"
            "请控制在 1-2 句内，尽量不超过 25 个汉字，适合直接朗读。",
        )
        msgs = [{"role": "system", "content": system_prompt}]
        _SESSIONS[device_id] = msgs
    return msgs


def _trim_session(msgs: list[dict], keep: int = 12) -> None:
    # Keep system + last N messages
    if len(msgs) <= 1 + keep:
        return
    system = msgs[0:1]
    tail = msgs[-keep:]
    msgs[:] = system + tail


def _qwen_chat(api_key: str, model: str, messages: list[dict]) -> str:
    url = f"{_dashscope_compat_base()}/chat/completions"
    max_tokens = int(os.getenv("QWEN_MAX_TOKENS", "160"))
    temperature = float(os.getenv("QWEN_TEMPERATURE", "0.3"))
    r = requests.post(
        url,
        headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
        json={
            "model": model,
            "messages": messages,
            "max_tokens": max_tokens,
            "temperature": temperature,
        },
        timeout=90,
    )
    if r.status_code != 200:
        raise HTTPException(status_code=502, detail=r.text)
    j = r.json()
    try:
        return (j["choices"][0]["message"]["content"] or "").strip()
    except Exception:
        raise HTTPException(status_code=502, detail=f"bad llm response: {j}")

def _qwen_chat_stream(api_key: str, model: str, messages: list[dict]):
    """
    Stream chat completion deltas (OpenAI-compatible SSE).

    Yields incremental text chunks (str).
    """
    url = f"{_dashscope_compat_base()}/chat/completions"
    max_tokens = int(os.getenv("QWEN_MAX_TOKENS", "160"))
    temperature = float(os.getenv("QWEN_TEMPERATURE", "0.3"))
    r = requests.post(
        url,
        headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
        json={
            "model": model,
            "messages": messages,
            "max_tokens": max_tokens,
            "temperature": temperature,
            "stream": True,
        },
        stream=True,
        timeout=90,
    )
    if r.status_code != 200:
        raise HTTPException(status_code=502, detail=r.text)

    for raw in r.iter_lines(decode_unicode=True):
        if not raw:
            continue
        line = raw.strip()
        if not line.startswith("data:"):
            continue
        payload = line[len("data:") :].strip()
        if not payload:
            continue
        if payload == "[DONE]":
            break
        try:
            j = json.loads(payload)
        except Exception:
            continue

        try:
            choice = (j.get("choices") or [])[0] or {}
            delta = choice.get("delta") or {}
            text = (delta.get("content") or "").strip("\r")
            if text:
                yield text
        except Exception:
            continue


def _qwen_asr(api_key: str, wav_bytes: bytes, model: str = "qwen3-asr-flash") -> str:
    # Preferred: OpenAI-compatible audio transcription endpoint.
    transcribe_url = f"{_dashscope_compat_base()}/audio/transcriptions"
    language = os.getenv("QWEN_ASR_LANGUAGE", "zh")
    r = requests.post(
        transcribe_url,
        headers={"Authorization": f"Bearer {api_key}"},
        files={"file": ("audio.wav", wav_bytes, "audio/wav")},
        data={"model": model, "language": language},
        timeout=90,
    )

    if r.status_code == 200:
        j = r.json()
        text = (j.get("text") or j.get("transcript") or "").strip()
        if not text:
            raise HTTPException(status_code=502, detail=f"bad asr response: {j}")
        return text

    # Fallback: some providers don't expose /audio/transcriptions but accept audio via chat/completions.
    if r.status_code in (404, 405):
        url = f"{_dashscope_compat_base()}/chat/completions"
        audio_data_url = "data:audio/wav;base64," + base64.b64encode(wav_bytes).decode("ascii")
        payload = {
            "model": model,
            "messages": [
                {
                    "role": "user",
                    "content": [
                        {"type": "input_audio", "input_audio": {"data": audio_data_url}},
                    ],
                }
            ],
        }
        rr = requests.post(
            url,
            headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
            json=payload,
            timeout=90,
        )
        if rr.status_code != 200:
            raise HTTPException(status_code=502, detail=rr.text)
        j = rr.json()
        try:
            return (j["choices"][0]["message"]["content"] or "").strip()
        except Exception:
            raise HTTPException(status_code=502, detail=f"bad asr response: {j}")

    raise HTTPException(status_code=502, detail=r.text)


def _qwen_tts(
    api_key: str,
    text: str,
    voice: str,
    model: str,
    sample_rate: int,
    language_type: str = "Chinese",
) -> bytes:
    # DashScope Qwen-TTS/Qwen3-TTS: non-streaming returns output.audio.url (WAV file URL).
    # Streaming returns base64 PCM fragments in output.audio.data.
    dashscope_req = {
        "model": model,
        "input": {"text": text, "voice": voice, "language_type": language_type},
        # Some models/versions ignore these; normalization below enforces final format.
        "parameters": {"sample_rate": sample_rate},
    }

    r = requests.post(
        _dashscope_tts_url(),
        headers={"Authorization": f"Bearer {api_key}", "Content-Type": "application/json"},
        json=dashscope_req,
        timeout=60,
    )

    if r.status_code != 200:
        raise HTTPException(status_code=502, detail=r.text)

    data = r.json()
    audio = (
        (((data.get("output") or {}).get("audio") or {}) if isinstance(data, dict) else {})
        if data is not None
        else {}
    )

    audio_url: str = (audio.get("url") or "").strip()
    if audio_url:
        ar = requests.get(audio_url, timeout=60)
        if ar.status_code != 200:
            raise HTTPException(status_code=502, detail=ar.text)
        wav_bytes = ar.content
        return _normalize_wav(wav_bytes, target_sr=sample_rate, target_channels=1)

    audio_b64: Optional[str] = audio.get("data")
    if not audio_b64:
        raise HTTPException(status_code=502, detail="missing output.audio.url/data")

    pcm = base64.b64decode(audio_b64)
    if pcm.startswith(b"RIFF"):
        return _normalize_wav(pcm, target_sr=sample_rate, target_channels=1)
    return _normalize_wav(_pcm_to_wav(pcm, int(sample_rate or 24000)), target_sr=sample_rate, target_channels=1)


@app.get("/health")
def health() -> dict:
    return {"ok": True}


@app.post("/tts")
async def tts(req: Request) -> Response:
    api_key = _env("DASHSCOPE_API_KEY")

    content_type = (req.headers.get("content-type") or "").lower()
    text: str = ""
    voice: str = "Cherry"
    model: str = "qwen3-tts-flash"
    language_type: str = "Chinese"
    sample_rate: int = int(os.getenv("QWEN_TTS_SAMPLE_RATE", "16000"))

    if "application/json" in content_type:
        payload = await req.json()
        text = (payload.get("text") or "").strip()
        voice = (payload.get("voice") or voice).strip()
        model = (payload.get("model") or model).strip()
        language_type = (payload.get("language_type") or language_type).strip()
        sample_rate = int(payload.get("sample_rate") or sample_rate)
    else:
        text = (await req.body()).decode("utf-8", errors="ignore").strip()

    if not text:
        raise HTTPException(status_code=400, detail="text is empty")

    wav = _qwen_tts(
        api_key=api_key,
        text=text,
        voice=voice,
        model=model,
        sample_rate=sample_rate,
        language_type=language_type,
    )
    return Response(content=wav, media_type="audio/wav")


@app.post("/chat")
async def chat(req: Request) -> Response:
    api_key = _env("DASHSCOPE_API_KEY")
    device_id = (req.headers.get("x-device-id") or "default").strip() or "default"

    content_type = (req.headers.get("content-type") or "").lower()
    if "audio/wav" not in content_type:
        raise HTTPException(status_code=400, detail="Content-Type must be audio/wav")

    wav_bytes = await req.body()
    if not wav_bytes:
        raise HTTPException(status_code=400, detail="empty wav")

    # 1) ASR
    asr_model = os.getenv("QWEN_ASR_MODEL", "qwen3-asr-flash")
    user_text = _qwen_asr(api_key=api_key, wav_bytes=wav_bytes, model=asr_model)

    # 2) LLM with session memory
    msgs = _session(device_id)
    # Keep replies short for natural voice chat + smaller audio
    user_text = user_text.strip()
    msgs.append({"role": "user", "content": user_text})
    _trim_session(msgs, keep=int(os.getenv("QWEN_MAX_TURNS", "12")))

    llm_model = os.getenv("QWEN_LLM_MODEL", "qwen-plus")
    assistant_text = _qwen_chat(api_key=api_key, model=llm_model, messages=msgs)
    if not assistant_text:
        assistant_text = "我想了一下，但不知道怎么回答。"

    msgs.append({"role": "assistant", "content": assistant_text})
    _trim_session(msgs, keep=int(os.getenv("QWEN_MAX_TURNS", "12")))

    # 3) TTS (downmixed to mono + resampled by _qwen_tts normalization)
    voice = os.getenv("QWEN_TTS_VOICE", "Cherry")
    tts_model = os.getenv("QWEN_TTS_MODEL", "qwen3-tts-flash")
    sample_rate = int(os.getenv("QWEN_TTS_SAMPLE_RATE", "16000"))
    language_type = os.getenv("QWEN_TTS_LANGUAGE_TYPE", "Chinese")
    wav = _qwen_tts(
        api_key=api_key,
        text=assistant_text,
        voice=voice,
        model=tts_model,
        sample_rate=sample_rate,
        language_type=language_type,
    )

    return Response(content=wav, media_type="audio/wav")


@app.post("/chat_pcm")
async def chat_pcm(req: Request) -> StreamingResponse:
    """
    Low-latency voice chat:
    - request:  audio/wav
    - response: streaming 16-bit PCM mono (audio/L16)

    This endpoint uses Qwen realtime TTS (WebSocket) to start sending audio as soon as possible.
    """
    api_key = _env("DASHSCOPE_API_KEY")
    device_id = (req.headers.get("x-device-id") or "default").strip() or "default"

    content_type = (req.headers.get("content-type") or "").lower()
    if "audio/wav" not in content_type:
        raise HTTPException(status_code=400, detail="Content-Type must be audio/wav")

    wav_bytes = await req.body()
    if not wav_bytes:
        raise HTTPException(status_code=400, detail="empty wav")

    # 1) ASR
    asr_model = os.getenv("QWEN_ASR_MODEL", "qwen3-asr-flash")
    user_text = _qwen_asr(api_key=api_key, wav_bytes=wav_bytes, model=asr_model).strip()

    # 2) LLM with session memory
    msgs = _session(device_id)
    msgs.append({"role": "user", "content": user_text})
    _trim_session(msgs, keep=int(os.getenv("QWEN_MAX_TURNS", "12")))

    # 3) Stream LLM output -> realtime TTS -> stream PCM
    llm_model = os.getenv("QWEN_LLM_MODEL", "qwen-plus")
    voice = os.getenv("QWEN_TTS_VOICE", "Cherry")
    tts_model = os.getenv("QWEN_TTS_REALTIME_MODEL", os.getenv("QWEN_TTS_MODEL", "qwen3-tts-flash-realtime"))
    target_sr = int(os.getenv("QWEN_TTS_SAMPLE_RATE", "16000"))

    try:
        in_sr, out_q, stop_evt, cb = _qwen_chat_to_realtime_tts_pcm_stream(
            api_key=api_key,
            msgs=msgs,
            llm_model=llm_model,
            voice=voice,
            tts_model=tts_model,
            target_sample_rate=target_sr,
        )
    except Exception as e:
        raise HTTPException(status_code=501, detail=str(e))

    def _iter_pcm():
        # Stream and (optionally) resample to target_sr.
        state = None
        while True:
            chunk = out_q.get()
            if chunk is None:
                break
            if not chunk:
                continue

            # realtime returns PCM 16-bit mono. Ensure even length.
            if len(chunk) % 2 == 1:
                chunk = chunk[:-1]
            if not chunk:
                continue

            if target_sr and in_sr and target_sr != in_sr:
                # audioop.ratecv keeps internal state for smooth resampling
                chunk, state = audioop.ratecv(chunk, 2, 1, in_sr, target_sr, state)
                if not chunk:
                    continue
            yield chunk

        stop_evt.set()
        if cb.error is not None:
            # Can't raise after headers; just end stream.
            return

    headers = {
        "X-Audio-Sample-Rate": str(target_sr),
        "X-Audio-Channels": "1",
        "X-Audio-Format": "S16LE",
    }
    media_type = f"audio/L16;rate={target_sr};channels=1"
    return StreamingResponse(_iter_pcm(), media_type=media_type, headers=headers)


# ==============================================================================
# WebSocket Streaming Endpoint (xiaozhi-compatible protocol)
# ==============================================================================

class WsSession:
    """Per-connection WebSocket session state."""
    def __init__(self, session_id: str):
        self.session_id = session_id
        self.device_id: str = "default"
        self.audio_buffer = io.BytesIO()
        self.sample_rate: int = 16000
        self.state: str = "idle"  # idle, listening, speaking
        self.stop_speaking = False

_ws_sessions: dict[str, WsSession] = {}


async def _ws_send_json(ws: WebSocket, msg: dict) -> bool:
    """Send JSON message to WebSocket client."""
    try:
        await ws.send_text(json.dumps(msg, ensure_ascii=False))
        return True
    except Exception:
        return False


async def _ws_send_audio(ws: WebSocket, pcm_bytes: bytes) -> bool:
    """Send binary PCM audio to WebSocket client."""
    try:
        await ws.send_bytes(pcm_bytes)
        return True
    except Exception:
        return False


async def _ws_handle_listen_start(ws: WebSocket, session: WsSession, mode: str):
    """Handle listen start: prepare to receive audio."""
    session.audio_buffer = io.BytesIO()
    session.state = "listening"
    print(f"[WS] Session {session.session_id}: start listening (mode={mode})")


async def _ws_handle_listen_stop(ws: WebSocket, session: WsSession):
    """Handle listen stop: run ASR + LLM + TTS pipeline."""
    if session.state != "listening":
        return

    session.state = "speaking"
    session.stop_speaking = False

    api_key = _env("DASHSCOPE_API_KEY")
    asr_model = os.getenv("QWEN_ASR_MODEL", "qwen3-asr-flash")
    llm_model = os.getenv("QWEN_LLM_MODEL", "qwen-plus")
    voice = os.getenv("QWEN_TTS_VOICE", "Cherry")
    tts_model = os.getenv("QWEN_TTS_REALTIME_MODEL", os.getenv("QWEN_TTS_MODEL", "qwen3-tts-flash-realtime"))
    target_sr = int(os.getenv("QWEN_TTS_SAMPLE_RATE", "16000"))

    # Get recorded audio
    pcm_bytes = session.audio_buffer.getvalue()
    if len(pcm_bytes) < 1600:  # less than 100ms at 16kHz 16-bit
        print(f"[WS] Session {session.session_id}: audio too short, skip")
        session.state = "idle"
        return

    # Convert to WAV for ASR
    wav_bytes = _pcm_to_wav(pcm_bytes, session.sample_rate)
    print(f"[WS] Session {session.session_id}: ASR with {len(wav_bytes)} bytes WAV")

    # 1) ASR
    try:
        user_text = _qwen_asr(api_key=api_key, wav_bytes=wav_bytes, model=asr_model).strip()
    except Exception as e:
        print(f"[WS] ASR error: {e}")
        session.state = "idle"
        return

    if not user_text:
        print(f"[WS] Session {session.session_id}: ASR returned empty")
        session.state = "idle"
        return

    # Send STT result
    await _ws_send_json(ws, {
        "session_id": session.session_id,
        "type": "stt",
        "text": user_text
    })
    print(f"[WS] Session {session.session_id}: STT = {user_text}")

    # 2) LLM + TTS with session memory
    msgs = _session(session.device_id)
    msgs.append({"role": "user", "content": user_text})
    _trim_session(msgs, keep=int(os.getenv("QWEN_MAX_TURNS", "12")))

    # Send TTS start
    await _ws_send_json(ws, {
        "session_id": session.session_id,
        "type": "tts",
        "state": "start"
    })

    # 3) Stream LLM -> TTS -> audio
    try:
        in_sr, out_q, stop_evt, cb = _qwen_chat_to_realtime_tts_pcm_stream(
            api_key=api_key,
            msgs=msgs,
            llm_model=llm_model,
            voice=voice,
            tts_model=tts_model,
            target_sample_rate=target_sr,
        )

        state = None
        while not session.stop_speaking:
            try:
                chunk = out_q.get(timeout=0.1)
            except queue.Empty:
                if stop_evt.is_set():
                    break
                continue

            if chunk is None:
                break
            if not chunk:
                continue

            # Ensure even length
            if len(chunk) % 2 == 1:
                chunk = chunk[:-1]
            if not chunk:
                continue

            # Resample if needed
            if target_sr and in_sr and target_sr != in_sr:
                chunk, state = audioop.ratecv(chunk, 2, 1, in_sr, target_sr, state)
                if not chunk:
                    continue

            # Send audio chunk
            if not await _ws_send_audio(ws, chunk):
                break

            # Yield to event loop
            await asyncio.sleep(0)

        stop_evt.set()

    except Exception as e:
        print(f"[WS] TTS error: {e}")

    # Send TTS stop
    await _ws_send_json(ws, {
        "session_id": session.session_id,
        "type": "tts",
        "state": "stop"
    })

    session.state = "idle"
    print(f"[WS] Session {session.session_id}: TTS done")


async def _ws_handle_abort(ws: WebSocket, session: WsSession):
    """Handle abort: stop current TTS playback."""
    session.stop_speaking = True
    print(f"[WS] Session {session.session_id}: abort requested")


@app.websocket("/ws")
async def websocket_endpoint(ws: WebSocket):
    """
    WebSocket streaming voice chat endpoint.
    
    Protocol (xiaozhi-compatible):
    1. Client connects
    2. Client sends: {"type": "hello", "version": 1, "audio_params": {...}}
    3. Server replies: {"type": "hello", "session_id": "xxx", "audio_params": {...}}
    4. Client sends: {"type": "listen", "state": "start", "mode": "auto"}
    5. Client sends binary PCM audio frames
    6. Client sends: {"type": "listen", "state": "stop"}
    7. Server sends: {"type": "stt", "text": "..."}
    8. Server sends: {"type": "tts", "state": "start"}
    9. Server sends binary PCM audio frames
    10. Server sends: {"type": "tts", "state": "stop"}
    """
    await ws.accept()
    
    session_id = str(uuid.uuid4())[:8]
    session = WsSession(session_id)
    _ws_sessions[session_id] = session
    
    device_id = ws.headers.get("device-id") or ws.headers.get("x-device-id") or "default"
    session.device_id = device_id
    
    print(f"[WS] New connection: session={session_id}, device={device_id}")
    
    try:
        while True:
            message = await ws.receive()
            
            if message["type"] == "websocket.disconnect":
                break
            
            if "text" in message:
                # JSON message
                try:
                    data = json.loads(message["text"])
                except json.JSONDecodeError:
                    continue
                
                msg_type = data.get("type", "")
                
                if msg_type == "hello":
                    # Client hello: extract audio params
                    audio_params = data.get("audio_params", {})
                    session.sample_rate = audio_params.get("sample_rate", 16000)
                    
                    # Reply with server hello
                    await _ws_send_json(ws, {
                        "type": "hello",
                        "transport": "websocket",
                        "session_id": session_id,
                        "audio_params": {
                            "format": "pcm",
                            "sample_rate": int(os.getenv("QWEN_TTS_SAMPLE_RATE", "16000")),
                            "channels": 1
                        }
                    })
                    print(f"[WS] Session {session_id}: hello handshake complete")
                
                elif msg_type == "listen":
                    state = data.get("state", "")
                    if state == "start":
                        mode = data.get("mode", "auto")
                        await _ws_handle_listen_start(ws, session, mode)
                    elif state == "stop":
                        await _ws_handle_listen_stop(ws, session)
                
                elif msg_type == "abort":
                    await _ws_handle_abort(ws, session)
            
            elif "bytes" in message:
                # Binary audio data
                if session.state == "listening":
                    session.audio_buffer.write(message["bytes"])
    
    except WebSocketDisconnect:
        print(f"[WS] Session {session_id}: client disconnected")
    except Exception as e:
        print(f"[WS] Session {session_id}: error: {e}")
    finally:
        _ws_sessions.pop(session_id, None)
        print(f"[WS] Session {session_id}: cleaned up")

