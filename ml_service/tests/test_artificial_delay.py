"""Proves the ML_SERVICE_ARTIFICIAL_DELAY_MS hook (main.py) actually
delays responses -- this is the mechanism Phase 7's C++-side
graceful-degradation test relies on to simulate a slow service, so it
needs its own direct proof, not just a code read.
"""

import time

import pytest
from fastapi.testclient import TestClient

from app.main import app

client = TestClient(app)

_PAYLOAD = {"account_id": "ACC-1", "instrument_id": "ACME", "window_features": {}}


def test_no_delay_by_default() -> None:
    start = time.perf_counter()
    response = client.post("/score", json=_PAYLOAD)
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    assert response.status_code == 200
    assert elapsed_ms < 200.0, f"expected a fast response with no delay configured, took {elapsed_ms:.1f}ms"


def test_artificial_delay_env_var_actually_delays_the_response(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("ML_SERVICE_ARTIFICIAL_DELAY_MS", "300")
    start = time.perf_counter()
    response = client.post("/score", json=_PAYLOAD)
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    assert response.status_code == 200
    assert elapsed_ms >= 300.0, f"expected the configured 300ms delay to actually elapse, took {elapsed_ms:.1f}ms"
    # Generous upper bound -- proves this is "delay by ~300ms", not
    # "something unrelated made this request slow."
    assert elapsed_ms < 1000.0, f"delay was much larger than configured, took {elapsed_ms:.1f}ms"


def test_delay_reverts_to_none_after_env_var_is_unset(monkeypatch: pytest.MonkeyPatch) -> None:
    monkeypatch.setenv("ML_SERVICE_ARTIFICIAL_DELAY_MS", "300")
    monkeypatch.delenv("ML_SERVICE_ARTIFICIAL_DELAY_MS")
    start = time.perf_counter()
    client.post("/score", json=_PAYLOAD)
    elapsed_ms = (time.perf_counter() - start) * 1000.0
    assert elapsed_ms < 200.0, f"expected the delay to be gone once the env var is unset, took {elapsed_ms:.1f}ms"
