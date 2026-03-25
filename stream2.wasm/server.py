"""
hyni voice scheduler - backend (with A/B provider switching and audio download)
"""

import json
import os
import re
from contextlib import asynccontextmanager
from pathlib import Path

import httpx
from dotenv import load_dotenv
from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse, Response
from fastapi.staticfiles import StaticFiles
from pydantic import BaseModel, Field

load_dotenv()

STATIC_DIR = Path(__file__).parent
LLM_TIMEOUT_S = float(os.environ.get("LLM_TIMEOUT_S", "30"))

FENCE_RE = re.compile(r"```(?:json)?|```")
ALLOWED_STATES = {"get_name", "get_datetime", "get_title", "confirming", "done"}


# ---------------------------------------------------------------------------
# LLM provider adapters
# ---------------------------------------------------------------------------

def _openai_request(messages, system, model):
    return {
        "url": "https://api.openai.com/v1/chat/completions",
        "headers": {"Authorization": f"Bearer {_env('LLM_API_KEY')}"},
        "json": {
            "model": model or "gpt-4o",
            "temperature": 0.3,
            "max_tokens": 400,
            "messages": [{"role": "system", "content": system}] + messages,
        },
        "extract": lambda d: d["choices"][0]["message"]["content"],
    }


def _anthropic_request(messages, system, model):
    return {
        "url": "https://api.anthropic.com/v1/messages",
        "headers": {
            "x-api-key": _env("LLM_API_KEY"),
            "anthropic-version": "2023-06-01",
        },
        "json": {
            "model": model or "claude-opus-4-6",
            "max_tokens": 400,
            "system": system,
            "messages": messages,
        },
        "extract": lambda d: d["content"][0]["text"],
    }


PROVIDERS = {
    "openai": _openai_request,
    "anthropic": _anthropic_request,
}


def _env(key: str, default: str = "") -> str:
    return os.environ.get(key, default)


# ---------------------------------------------------------------------------
# Runtime-switchable provider state
# ---------------------------------------------------------------------------

_active_provider: str = _env("LLM_PROVIDER", "openai")
_last_audio_buffer: bytes | None = None


# ---------------------------------------------------------------------------
# System prompt
# ---------------------------------------------------------------------------

def build_system_prompt(today: str, tz: str) -> str:
    return f"""You are a concise voice scheduling assistant.
Collect three things from the user:
1. Their name
2. Preferred date and time for a meeting
3. Optional meeting title (default: "Meeting")

FIRST TURN:
The very first user message is always the conversation opener. On that turn ONLY,
open with a warm 2-3 sentence greeting that introduces yourself and what you do,
then ask for their name. Example shape (rephrase naturally, do not copy verbatim):
"Hi, I'm your scheduling assistant. I can help you set up a calendar event.
What's your name?" Set next_state to get_name.

RULES (all turns after the first):
- Voice conversation: keep ALL responses to 1-2 sentences maximum.
- Do not repeat or summarise what the user said back to them.
- Move on immediately once a slot is filled.
- Today is {today}. The user's timezone is {tz}. Convert relative expressions to ISO 8601 local time (no Z suffix).
- Respond ONLY with raw JSON - no markdown fences, no preamble, no explanation:
{{
  "speech":     "What you say aloud to the user",
  "slots": {{
    "name":     null or "string",
    "dateTime": null or "ISO 8601 e.g. 2025-06-15T14:00:00",
    "title":    null or "string"
  }},
  "next_state": "get_name" | "get_datetime" | "get_title" | "confirming" | "done"
}}

STATE GUIDE:
- get_name     -> still need the user's name
- get_datetime -> still need date/time
- get_title    -> ask for optional title (skip straight to confirming if already have one)
- confirming   -> read back all three slots, ask "Does that sound right?"
                  ALWAYS speak the resolved ABSOLUTE date and time (e.g. "March 22nd at 7 PM"),
                  never a relative expression like "tomorrow" - the user needs to verify
                  you resolved it correctly.
- done         -> user confirmed (said yes / correct / sounds good)

Always carry all filled slots forward in every response.
If the user corrects a slot mid-confirmation, update it and return to confirming."""


# ---------------------------------------------------------------------------
# Request / response models
# ---------------------------------------------------------------------------

class ChatMessage(BaseModel):
    role: str
    content: str


class ChatRequest(BaseModel):
    history: list[ChatMessage] = Field(min_length=1)
    today: str
    tz: str


class ProviderRequest(BaseModel):
    provider: str


# ---------------------------------------------------------------------------
# App + lifespan-managed HTTP client
# ---------------------------------------------------------------------------

@asynccontextmanager
async def lifespan(app: FastAPI):
    app.state.http = httpx.AsyncClient(timeout=LLM_TIMEOUT_S)
    yield
    await app.state.http.aclose()


app = FastAPI(title="hyni-scheduler", lifespan=lifespan)


@app.middleware("http")
async def coop_coep_headers(request, call_next):
    response = await call_next(request)
    response.headers["Cross-Origin-Opener-Policy"] = "same-origin"
    response.headers["Cross-Origin-Embedder-Policy"] = "credentialless"
    return response


# ---------------------------------------------------------------------------
# API routes
# ---------------------------------------------------------------------------

@app.get("/api/health")
async def health():
    return {"status": "ok"}


@app.get("/api/config")
async def config():
    return {
        "googleClientId": _env("GOOGLE_CLIENT_ID"),
        "llmConfigured": bool(_env("LLM_API_KEY")),
        "llmProvider": _active_provider,
    }


@app.post("/api/ab/provider")
async def set_provider(req: ProviderRequest):
    global _active_provider
    if req.provider not in PROVIDERS:
        raise HTTPException(
            status_code=422,
            detail=f"Unknown provider '{req.provider}'. Must be one of: {list(PROVIDERS)}",
        )
    _active_provider = req.provider
    return {"provider": _active_provider}


@app.post("/api/ab/audio")
async def store_audio(request: Request):
    global _last_audio_buffer
    _last_audio_buffer = await request.body()
    return {"stored": len(_last_audio_buffer)}


@app.get("/api/ab/audio")
async def get_audio():
    if _last_audio_buffer is None:
        raise HTTPException(status_code=404, detail="No audio buffer stored yet")
    return Response(
        content=_last_audio_buffer,
        media_type="application/octet-stream",
    )


@app.post("/api/chat")
async def chat(req: ChatRequest):
    api_key = _env("LLM_API_KEY")
    if not api_key:
        raise HTTPException(status_code=503, detail="LLM_API_KEY not configured")

    if _active_provider not in PROVIDERS:
        raise HTTPException(status_code=500, detail=f"Unknown provider: {_active_provider}")

    system = build_system_prompt(req.today, req.tz)
    messages = [m.model_dump() for m in req.history]
    spec = PROVIDERS[_active_provider](messages, system, _env("LLM_MODEL") or None)

    try:
        resp = await app.state.http.post(
            spec["url"], headers=spec["headers"], json=spec["json"]
        )
    except httpx.HTTPError as e:
        raise HTTPException(status_code=502, detail=f"Upstream request failed: {e}")

    if resp.status_code >= 400:
        body = resp.text[:500]
        raise HTTPException(
            status_code=502, detail=f"Upstream {resp.status_code}: {body}"
        )

    try:
        raw_text = spec["extract"](resp.json())
    except (KeyError, IndexError, json.JSONDecodeError) as e:
        raise HTTPException(
            status_code=502, detail=f"Unexpected upstream response shape: {e}"
        )

    cleaned = FENCE_RE.sub("", raw_text).strip()
    try:
        parsed = json.loads(cleaned)
    except json.JSONDecodeError:
        raise HTTPException(
            status_code=500, detail=f"LLM returned non-JSON: {raw_text[:300]}"
        )

    for key in ("speech", "slots", "next_state"):
        if key not in parsed:
            raise HTTPException(
                status_code=500, detail=f"LLM response missing '{key}': {cleaned[:300]}"
            )
    if parsed["next_state"] not in ALLOWED_STATES:
        raise HTTPException(
            status_code=500,
            detail=f"LLM returned invalid next_state: {parsed['next_state']}",
        )

    if parsed["next_state"] == "done":
        slots = parsed.get("slots") or {}
        if not slots.get("name") or not slots.get("dateTime"):
            raise HTTPException(
                status_code=500,
                detail=(
                    "LLM returned done state but required slots are missing "
                    "(need name and dateTime)"
                ),
            )

    return JSONResponse(parsed)


# ---------------------------------------------------------------------------
# Static files (mounted last so /api/* routes win)
# ---------------------------------------------------------------------------

app.mount("/", StaticFiles(directory=STATIC_DIR, html=True), name="static")


if __name__ == "__main__":
    import uvicorn
    uvicorn.run(app, host="0.0.0.0", port=8000)
