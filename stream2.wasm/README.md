# Voice Scheduling Agent

<img width="1049" height="496" alt="vsa" src="https://github.com/user-attachments/assets/c61f51f5-6228-4cbf-a91e-aafa5fb7f9f3" />

A real-time voice scheduling assistant that runs entirely in the browser. Speak naturally to book a meeting — the agent collects your name, preferred date/time, and an optional title, confirms the details aloud, then creates the event on your Google Calendar.

## Deployed URL

**https://vsa-p2c4.onrender.com/**

> First load may take 30–60 seconds if the Render instance is cold.

## How to Test

1. Open the deployed URL in Chrome or Edge (required for SharedArrayBuffer + Web Speech API).
2. Select a Whisper model — **base.en** is recommended for best accuracy/speed balance.
3. Wait for the model to download and load into WASM.
4. Click **⚿ Auth Google Calendar** and complete the OAuth consent flow.
5. Click **▶ Start Session** — the agent will greet you and ask for your name.
6. Speak naturally: give your name, a date/time ("tomorrow at 3 PM", "March 25th at 10 AM"), and optionally a meeting title.
7. The agent reads back the details — say "yes" or "correct" to confirm.
8. The event is created on your Google Calendar with a direct link in the toast notification.

See [INSTRUCTIONS.md](INSTRUCTIONS.md) for detailed setup, deployment, troubleshooting, and local development.

## Architecture

```
┌─────────────────────────────────────────────────────────┐
│  Browser                                                │
│                                                         │
│  Microphone → WebAudio (high-pass + VAD/continuous)     │
│            → Whisper.cpp WASM (ASR)                     │
│            → Agent state machine (JS)                   │
│            → POST /api/chat (LLM slot extraction)       │
│            → Web Speech API (TTS response)              │
│            → Google Calendar API (event creation)       │
│                                                         │
└──────────────────────┬──────────────────────────────────┘
                       │ /api/chat
┌──────────────────────▼──────────────────────────────────┐
│  FastAPI Backend (Render)                               │
│                                                         │
│  - Proxies LLM calls (OpenAI or Anthropic)              │
│  - Serves GOOGLE_CLIENT_ID via /api/config              │
│  - Sets COOP/COEP headers for SharedArrayBuffer         │
│  - Serves static WASM frontend                          │
│                                                         │
└─────────────────────────────────────────────────────────┘
```

**Speech-to-text**: Whisper.cpp compiled to WebAssembly via Emscripten with pthread support. Runs on-device — no audio leaves the browser. Models are fetched from HuggingFace and cached in IndexedDB.

**LLM agent**: A structured slot-filling loop. Each user utterance is sent (along with full conversation history) to the LLM, which returns a JSON object with `speech`, `slots` (name, dateTime, title), and `next_state`. The backend validates the contract and guards against premature state transitions.

**Text-to-speech**: Web Speech API (`SpeechSynthesisUtterance`). The agent pauses ASR polling while speaking to avoid feeding its own voice back into the transcription loop.

**Calendar integration**: Browser-side OAuth2 implicit token flow via Google Identity Services (GIS). The backend only serves the public `GOOGLE_CLIENT_ID` — no client secret is involved, and calendar writes happen directly from the browser to the Google Calendar API v3. The access token is held in memory and never sent to the backend.

## Repository Structure

```
├── README.md                  ← you are here
├── INSTRUCTIONS.md            ← setup, deployment, local dev, troubleshooting
├── render.yaml                ← Render deploy blueprint
└── stream2.wasm/
    ├── index.html             ← single-page frontend (ASR + agent + calendar)
    ├── server.py              ← FastAPI backend (LLM proxy + static serving)
    ├── test_server.py         ← pytest suite for backend
    ├── requirements.txt       ← Python dependencies
    ├── libstream.js           ← Whisper.cpp Emscripten glue
    ├── libstream.wasm         ← Whisper.cpp WASM binary
    ├── libstream.worker.js    ← pthread worker
    └── helpers.js             ← IndexedDB model caching
```

## Tech Stack

| Layer | Technology |
|-------|-----------|
| ASR | Whisper.cpp (WASM + pthreads) |
| LLM | OpenAI GPT-4o or Anthropic Claude (configurable) |
| TTS | Web Speech API |
| Calendar | Google Calendar API v3 + GIS OAuth2 |
| Backend | FastAPI + uvicorn + httpx |
| Hosting | Render (Web Service) |

## Environment Variables

| Variable | Required | Default | Description |
|----------|----------|---------|-------------|
| `LLM_API_KEY` | Yes | — | OpenAI or Anthropic API key |
| `LLM_PROVIDER` | No | `openai` | `openai` or `anthropic` |
| `LLM_MODEL` | No | `gpt-4o` / `claude-opus-4-6` | Override default model |
| `GOOGLE_CLIENT_ID` | Yes | — | Google OAuth2 client ID |
| `LLM_TIMEOUT_S` | No | `30` | Upstream LLM timeout in seconds |

## Notes

- Requires a Chromium-based browser (Chrome, Edge) for SharedArrayBuffer and reliable Web Speech API support.
- COOP/COEP headers are set by the backend middleware on all responses.
- The WASM ASR module (`libstream.js/wasm`) is from the [whisper.cpp](https://github.com/ggerganov/whisper.cpp) project, compiled with Emscripten for browser use.
- For local development, use `http://localhost:8000` (not `0.0.0.0:8000`) as the origin must match your Google OAuth config.
