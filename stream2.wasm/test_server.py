"""
Backend tests for server.py.

Upstream LLM calls are mocked with respx; nothing hits the network.
Env vars are patched per-test so suite order is irrelevant.
"""

import json

import pytest
import respx
from fastapi.testclient import TestClient
from httpx import Response

import server


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

@pytest.fixture
def env(monkeypatch):
    """Baseline happy-path env. Individual tests override via monkeypatch."""
    monkeypatch.setenv("LLM_PROVIDER", "openai")
    monkeypatch.setenv("LLM_API_KEY", "sk-test-xxxxxxxx")
    monkeypatch.setenv("GOOGLE_CLIENT_ID", "123-abc.apps.googleusercontent.com")
    monkeypatch.delenv("LLM_MODEL", raising=False)
    return monkeypatch


@pytest.fixture
def client(env):
    # Context manager so the lifespan (AsyncClient setup/teardown) runs.
    with TestClient(server.app) as c:
        yield c


def _chat_body(user_msg="My name is Alex"):
    return {
        "history": [{"role": "user", "content": user_msg}],
        "today": "Friday, March 21, 2026",
        "tz": "America/New_York",
    }


def _openai_reply(content: str):
    """Wrap content in the OpenAI chat.completions envelope."""
    return {"choices": [{"message": {"content": content}}]}


def _anthropic_reply(content: str):
    """Wrap content in the Anthropic messages envelope."""
    return {"content": [{"text": content}]}


VALID_LLM_JSON = json.dumps({
    "speech": "Nice to meet you Alex. When would you like to meet?",
    "slots": {"name": "Alex", "dateTime": None, "title": None},
    "next_state": "get_datetime",
})


# ---------------------------------------------------------------------------
# /api/health
# ---------------------------------------------------------------------------

def test_health(client):
    r = client.get("/api/health")
    assert r.status_code == 200
    assert r.json() == {"status": "ok"}


# ---------------------------------------------------------------------------
# /api/config
# ---------------------------------------------------------------------------

def test_config_returns_client_id(client):
    r = client.get("/api/config")
    assert r.status_code == 200
    body = r.json()
    assert body["googleClientId"] == "123-abc.apps.googleusercontent.com"
    assert body["llmConfigured"] is True


def test_config_empty_when_unset(env, monkeypatch):
    monkeypatch.delenv("GOOGLE_CLIENT_ID", raising=False)
    monkeypatch.delenv("LLM_API_KEY", raising=False)
    with TestClient(server.app) as c:
        body = c.get("/api/config").json()
    assert body["googleClientId"] == ""
    assert body["llmConfigured"] is False


# ---------------------------------------------------------------------------
# COOP / COEP headers
# ---------------------------------------------------------------------------

def test_static_has_coop_coep(client):
    r = client.get("/index.html")
    assert r.status_code == 200
    assert r.headers["cross-origin-opener-policy"] == "same-origin"
    assert r.headers["cross-origin-embedder-policy"] == "credentialless"


def test_api_routes_also_have_coop_coep(client):
    r = client.get("/api/health")
    assert r.headers["cross-origin-opener-policy"] == "same-origin"
    assert r.headers["cross-origin-embedder-policy"] == "credentialless"


# ---------------------------------------------------------------------------
# /api/chat - happy paths
# ---------------------------------------------------------------------------

@respx.mock
def test_chat_happy_path_openai(client):
    route = respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(200, json=_openai_reply(VALID_LLM_JSON))
    )

    r = client.post("/api/chat", json=_chat_body())

    assert r.status_code == 200
    body = r.json()
    assert body["speech"].startswith("Nice to meet you")
    assert body["slots"]["name"] == "Alex"
    assert body["next_state"] == "get_datetime"
    assert route.called


@respx.mock
def test_chat_happy_path_anthropic(env, monkeypatch):
    monkeypatch.setenv("LLM_PROVIDER", "anthropic")
    route = respx.post("https://api.anthropic.com/v1/messages").mock(
        return_value=Response(200, json=_anthropic_reply(VALID_LLM_JSON))
    )

    with TestClient(server.app) as c:
        r = c.post("/api/chat", json=_chat_body())

    assert r.status_code == 200
    assert r.json()["next_state"] == "get_datetime"
    assert route.called
    # verify the right auth header shape went upstream
    sent = route.calls.last.request
    assert sent.headers.get("x-api-key") == "sk-test-xxxxxxxx"
    assert sent.headers.get("anthropic-version") == "2023-06-01"


@respx.mock
def test_chat_strips_markdown_fences(client):
    fenced = f"```json\n{VALID_LLM_JSON}\n```"
    respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(200, json=_openai_reply(fenced))
    )

    r = client.post("/api/chat", json=_chat_body())

    assert r.status_code == 200
    assert r.json()["slots"]["name"] == "Alex"


@respx.mock
def test_chat_sysprompt_injects_today_and_tz(client):
    route = respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(200, json=_openai_reply(VALID_LLM_JSON))
    )

    client.post("/api/chat", json=_chat_body())

    sent_body = json.loads(route.calls.last.request.content)
    system_msg = sent_body["messages"][0]
    assert system_msg["role"] == "system"
    assert "Friday, March 21, 2026" in system_msg["content"]
    assert "America/New_York" in system_msg["content"]


@respx.mock
def test_chat_sysprompt_has_greeting_and_absolute_date_guidance(client):
    route = respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(200, json=_openai_reply(VALID_LLM_JSON))
    )

    client.post("/api/chat", json=_chat_body())

    sys = json.loads(route.calls.last.request.content)["messages"][0]["content"]
    # first-turn greeting guidance
    assert "FIRST TURN" in sys
    assert "introduces yourself" in sys
    assert "get_name" in sys
    # confirmation speaks absolute dates
    assert "ABSOLUTE" in sys
    assert "never a relative" in sys


@respx.mock
def test_chat_forwards_history(client):
    route = respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(200, json=_openai_reply(VALID_LLM_JSON))
    )

    body = {
        "history": [
            {"role": "user", "content": "Hello"},
            {"role": "assistant", "content": '{"speech":"Hi! What is your name?",...}'},
            {"role": "user", "content": "My name is Alex"},
        ],
        "today": "Friday, March 21, 2026",
        "tz": "UTC",
    }
    client.post("/api/chat", json=body)

    sent = json.loads(route.calls.last.request.content)
    # system + 3 history messages
    assert len(sent["messages"]) == 4
    assert sent["messages"][1]["content"] == "Hello"
    assert sent["messages"][3]["content"] == "My name is Alex"


@respx.mock
def test_chat_uses_model_override(env, monkeypatch):
    monkeypatch.setenv("LLM_MODEL", "gpt-4o-mini")
    route = respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(200, json=_openai_reply(VALID_LLM_JSON))
    )

    with TestClient(server.app) as c:
        c.post("/api/chat", json=_chat_body())

    sent = json.loads(route.calls.last.request.content)
    assert sent["model"] == "gpt-4o-mini"


# ---------------------------------------------------------------------------
# /api/chat - error paths
# ---------------------------------------------------------------------------

def test_chat_no_api_key(env, monkeypatch):
    monkeypatch.delenv("LLM_API_KEY", raising=False)
    with TestClient(server.app) as c:
        r = c.post("/api/chat", json=_chat_body())
    assert r.status_code == 503
    assert "LLM_API_KEY" in r.json()["detail"]


def test_chat_unknown_provider(env, monkeypatch):
    monkeypatch.setenv("LLM_PROVIDER", "cohere")
    with TestClient(server.app) as c:
        r = c.post("/api/chat", json=_chat_body())
    assert r.status_code == 500
    assert "Unknown LLM_PROVIDER" in r.json()["detail"]


@respx.mock
def test_chat_upstream_http_error(client):
    respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(429, json={"error": {"message": "rate limited"}})
    )
    r = client.post("/api/chat", json=_chat_body())
    assert r.status_code == 502
    assert "429" in r.json()["detail"]


@respx.mock
def test_chat_upstream_bad_shape(client):
    # valid HTTP but missing choices[0].message.content
    respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(200, json={"choices": []})
    )
    r = client.post("/api/chat", json=_chat_body())
    assert r.status_code == 502
    assert "Unexpected upstream response shape" in r.json()["detail"]


@respx.mock
def test_chat_llm_returns_prose(client):
    respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(200, json=_openai_reply("Sure, I'd be happy to help!"))
    )
    r = client.post("/api/chat", json=_chat_body())
    assert r.status_code == 500
    assert "non-JSON" in r.json()["detail"]


@respx.mock
def test_chat_llm_missing_required_key(client):
    bad = json.dumps({"speech": "hi", "slots": {}})  # no next_state
    respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(200, json=_openai_reply(bad))
    )
    r = client.post("/api/chat", json=_chat_body())
    assert r.status_code == 500
    assert "next_state" in r.json()["detail"]


@respx.mock
def test_chat_llm_invalid_state(client):
    bad = json.dumps({
        "speech": "hi",
        "slots": {"name": None, "dateTime": None, "title": None},
        "next_state": "teleporting",
    })
    respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(200, json=_openai_reply(bad))
    )
    r = client.post("/api/chat", json=_chat_body())
    assert r.status_code == 500
    assert "invalid next_state" in r.json()["detail"]


def test_chat_rejects_empty_history(client):
    r = client.post("/api/chat", json={"history": [], "today": "x", "tz": "UTC"})
    assert r.status_code == 422  # pydantic validation


# ---------------------------------------------------------------------------
# Premature 'done' guard - regression tests for the event-creation bug
# ---------------------------------------------------------------------------

@respx.mock
def test_chat_rejects_done_when_all_slots_empty(client):
    """LLM returns done but no slots filled at all - must be rejected."""
    bad = json.dumps({
        "speech": "All set, creating now!",
        "slots": {"name": None, "dateTime": None, "title": None},
        "next_state": "done",
    })
    respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(200, json=_openai_reply(bad))
    )
    r = client.post("/api/chat", json=_chat_body())
    assert r.status_code == 500
    assert "done state" in r.json()["detail"]
    assert "missing" in r.json()["detail"]


@respx.mock
def test_chat_rejects_done_when_datetime_missing(client):
    """Name collected but dateTime still null - creation must be blocked."""
    bad = json.dumps({
        "speech": "Great, creating your event!",
        "slots": {"name": "Alex", "dateTime": None, "title": None},
        "next_state": "done",
    })
    respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(200, json=_openai_reply(bad))
    )
    r = client.post("/api/chat", json=_chat_body())
    assert r.status_code == 500
    assert "done state" in r.json()["detail"]
    assert "missing" in r.json()["detail"]


@respx.mock
def test_chat_rejects_done_when_name_missing(client):
    """dateTime collected but name still null - creation must be blocked."""
    bad = json.dumps({
        "speech": "Creating the event now!",
        "slots": {"name": None, "dateTime": "2026-03-22T19:00:00", "title": None},
        "next_state": "done",
    })
    respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(200, json=_openai_reply(bad))
    )
    r = client.post("/api/chat", json=_chat_body())
    assert r.status_code == 500
    assert "done state" in r.json()["detail"]
    assert "missing" in r.json()["detail"]


@respx.mock
def test_chat_allows_done_with_complete_slots(client):
    """done with both name and dateTime present must pass validation."""
    good = json.dumps({
        "speech": "All confirmed! Creating your event.",
        "slots": {"name": "Alex", "dateTime": "2026-03-22T19:00:00", "title": "Standup"},
        "next_state": "done",
    })
    respx.post("https://api.openai.com/v1/chat/completions").mock(
        return_value=Response(200, json=_openai_reply(good))
    )
    r = client.post("/api/chat", json=_chat_body())
    assert r.status_code == 200
    body = r.json()
    assert body["next_state"] == "done"
    assert body["slots"]["name"] == "Alex"
    assert body["slots"]["dateTime"] == "2026-03-22T19:00:00"
