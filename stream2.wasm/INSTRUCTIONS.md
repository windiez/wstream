# Instructions — Voice Scheduling Agent

Detailed setup, deployment, local development, and troubleshooting guide.

## Table of Contents

- [Quick Start (Deployed)](#quick-start-deployed)
- [Local Development](#local-development)
- [Deploying to Render](#deploying-to-render)
- [Google OAuth Setup](#google-oauth-setup)
- [Calendar Integration Details](#calendar-integration-details)
- [ASR Configuration](#asr-configuration)
- [LLM Provider Configuration](#llm-provider-configuration)
- [Troubleshooting](#troubleshooting)

---

## Quick Start (Deployed)

The app is live at **https://vsa-p2c4.onrender.com/**

1. Open the URL in **Chrome** or **Edge** (Firefox and Safari lack full SharedArrayBuffer + Web Speech API support).
2. Select a Whisper model:
   - **tiny.en** (~31 MB) — fastest, lower accuracy
   - **base.en** (~57 MB) — recommended balance
   - **small.en** (~182 MB) — best accuracy, slower on low-end hardware
3. Wait for the model to download. It is cached in IndexedDB so subsequent visits skip the download.
4. Click **⚿ Auth Google Calendar** and authorize with your Google account.
5. Click **▶ Start Session** to begin. The agent will greet you via TTS.
6. Speak naturally — the agent will ask for your name, date/time, and optional meeting title.
7. Confirm when prompted. The event is created and a link to Google Calendar appears.

---

## Local Development

### Prerequisites

- Python 3.11+
- A modern Chromium-based browser

### Setup

```bash
git clone https://github.com/windiez/vsa.git
cd vsa/stream2.wasm

python -m venv .venv
source .venv/bin/activate        # Linux/macOS
# .venv\Scripts\activate         # Windows

pip install -r requirements.txt

cp .env.example .env
```

Edit `.env` and set:

```
LLM_API_KEY=sk-...              # OpenAI key (or Anthropic key with LLM_PROVIDER=anthropic)
GOOGLE_CLIENT_ID=xxxx.apps.googleusercontent.com
```

### Run

```bash
uvicorn server:app --host 0.0.0.0 --port 8000
```

Open **http://localhost:8000** in Chrome. Do not use `http://0.0.0.0:8000` — the origin must match your Google OAuth authorized JavaScript origins.

### Run Tests

```bash
pytest test_server.py -v
```

The test suite uses `respx` to mock upstream LLM calls, so no API key is needed for testing.

---

## Deploying to Render

1. Push the repo to GitHub.
2. In [Render](https://render.com), create a new **Web Service** from the repo.
3. Render auto-detects `render.yaml` and configures the build/start commands.
4. Set environment variables in the Render dashboard:
   - `LLM_API_KEY` (required)
   - `GOOGLE_CLIENT_ID` (required)
   - `LLM_PROVIDER` (optional, defaults to `openai`)
5. Deploy. Note your service URL (e.g., `https://vsa-p2c4.onrender.com`).
6. Add the Render URL to your Google OAuth authorized JavaScript origins (see below).

### Render Blueprint (`render.yaml`)

The blueprint configures a single web service that:
- Installs Python dependencies from `stream2.wasm/requirements.txt`
- Starts uvicorn serving the FastAPI app
- Exposes port 8000

---

## Google OAuth Setup

Calendar integration requires a Google OAuth2 **Web application** client.

### Create the OAuth Client

1. Go to [Google Cloud Console → APIs & Services → Credentials](https://console.cloud.google.com/apis/credentials).
2. Click **Create Credentials → OAuth client ID**.
3. Application type: **Web application**.
4. Under **Authorized JavaScript origins**, add:
   - `https://vsa-p2c4.onrender.com` (your deployed URL)
   - `http://localhost:8000` (for local development)
5. No redirect URIs are needed — we use the implicit token flow.
6. Copy the **Client ID** and set it as `GOOGLE_CLIENT_ID` in your environment.

### Enable the Calendar API

1. In Google Cloud Console, go to **APIs & Services → Library**.
2. Search for **Google Calendar API** and click **Enable**.

### OAuth Consent Screen

If your app is in "Testing" mode, only test users you've explicitly added can authorize. To allow any Google account, submit for verification or add individual test users in the OAuth consent screen settings.

---

## Calendar Integration Details

The calendar integration is entirely browser-side:

1. **Initialization**: On page load, the frontend calls `GET /api/config` to retrieve the `GOOGLE_CLIENT_ID`. The backend never sees or handles OAuth tokens.

2. **Authorization**: When the user clicks "Auth Google Calendar", the Google Identity Services (GIS) library opens a consent popup. The user grants `calendar.events` scope. The access token is stored in-memory in the browser (never persisted, never sent to the backend).

3. **Event Creation**: When the agent reaches the `done` state with all slots filled, the frontend POSTs directly to `https://www.googleapis.com/calendar/v3/calendars/primary/events` with the bearer token. The event includes:
   - Summary (meeting title or "Meeting")
   - Start/end time (1-hour duration, user's local timezone)
   - Description noting the attendee name

4. **Security**: No client secret is used. The token has a short expiry (~1 hour). Re-authorization is prompted automatically if the token expires mid-session.

---

## ASR Configuration

The app uses [Whisper.cpp](https://github.com/ggerganov/whisper.cpp) compiled to WebAssembly with Emscripten pthread support.

### Model Selection

| Model | Size | Speed | Accuracy | Best For |
|-------|------|-------|----------|----------|
| tiny.en | ~31 MB | Fast | Lower | Quick testing, strong accents |
| base.en | ~57 MB | Medium | Good | **Recommended for general use** |
| small.en | ~182 MB | Slower | Best | Noisy environments, precision-critical |

Models are quantized (q5_1) and downloaded from HuggingFace on first use, then cached in IndexedDB.

### Capture Modes

The app offers two audio capture modes, toggled via the **Use VAD** checkbox:

- **VAD ON** (default): Voice Activity Detection triggers Whisper only when speech is detected. Includes a 300ms pre-roll buffer to capture word onsets and silence-debounce capture for trailing sounds. Lower latency for conversational use.

- **VAD OFF** (continuous): Buffers all mic audio and flushes to Whisper on a fixed 3-second interval with 500ms overlap between chunks. More robust in noisy environments but higher latency.

### Audio Pipeline

The browser audio pipeline applies the following before Whisper processing:
1. **High-pass filter** (80 Hz) — removes DC offset, low-frequency rumble, and HVAC hum
2. **Amplitude normalization** — scales peak amplitude to 1.0 (capped at 10x gain) for consistent Whisper input levels
3. **Energy gate** — discards near-silent VAD segments (RMS < 0.005) before they reach WASM
4. **Transcript filtering** — discards blank, whitespace-only, or hallucination artifacts (`[BLANK_AUDIO]`, punctuation-only, etc.)

---

## LLM Provider Configuration

The backend proxies LLM calls so API keys stay server-side.

### OpenAI (default)

```env
LLM_PROVIDER=openai
LLM_API_KEY=sk-...
LLM_MODEL=gpt-4o          # optional, defaults to gpt-4o
```

### Anthropic

```env
LLM_PROVIDER=anthropic
LLM_API_KEY=sk-ant-...
LLM_MODEL=claude-opus-4-6  # optional, defaults to claude-opus-4-6
```

### LLM Contract

The LLM is instructed to return raw JSON on every turn:

```json
{
  "speech": "What you say aloud to the user",
  "slots": {
    "name": null,
    "dateTime": null,
    "title": null
  },
  "next_state": "get_name"
}
```

The backend validates: all three keys must be present, `next_state` must be one of `get_name`, `get_datetime`, `get_title`, `confirming`, or `done`, and `done` is rejected unless both `name` and `dateTime` slots are filled. This prevents prompt injection or hallucination from triggering premature event creation.

---

## Troubleshooting

### "SharedArrayBuffer is not defined"

The app requires `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy: credentialless` headers. The FastAPI backend sets these on all responses. If you see this error:
- Make sure you're accessing the app through the backend (`http://localhost:8000`), not by opening `index.html` directly.
- Use Chrome or Edge. Firefox support for `credentialless` COEP varies.

### Model download stalls or fails

- Check your network connection. Models are fetched from `huggingface.co`.
- Try a smaller model (tiny.en) first.
- Clear the browser's IndexedDB storage for the site and retry.

### No transcription appears

- Ensure microphone permission is granted (check the address bar icon).
- Verify the model loaded successfully (model status should show "Loaded").
- Check the Console Output panel for WASM errors.
- Try switching between VAD ON and VAD OFF modes.

### Agent doesn't respond

- Check the backend status panel at the bottom. "LLM: ready" must show.
- Verify `LLM_API_KEY` is set correctly in the environment.
- Check browser DevTools Network tab for `/api/chat` errors.

### Calendar auth fails

- Ensure `GOOGLE_CLIENT_ID` is set and the OAuth client has the correct authorized JavaScript origins.
- If your Google Cloud app is in "Testing" mode, your Google account must be listed as a test user.
- Check the browser console for GIS errors.

### TTS not working

- Web Speech API requires a user gesture before first playback. Clicking "Start Session" counts as one.
- Some browsers don't ship with English voices. Check `speechSynthesis.getVoices()` in the console.
- On mobile, TTS may be blocked by the OS if the device is in silent mode.

### Whisper produces poor accuracy

- Use **base.en** or **small.en** instead of tiny.en.
- Speak clearly at a normal pace, close to the microphone.
- Reduce background noise — the high-pass filter handles rumble, but Whisper struggles with overlapping speech or music.
- Try VAD OFF mode if short utterances are being missed.
