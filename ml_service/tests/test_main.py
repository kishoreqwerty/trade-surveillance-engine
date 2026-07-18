from fastapi.testclient import TestClient

from app.main import app

client = TestClient(app)


def test_health() -> None:
    response = client.get("/health")
    assert response.status_code == 200
    assert response.json() == {"status": "ok"}


def test_score_valid_request_returns_score_and_version() -> None:
    response = client.post(
        "/score",
        json={
            "account_id": "ACC-1",
            "instrument_id": "ACME",
            "window_features": {"order_count": 25.0, "total_qty": 3750.0},
        },
    )
    assert response.status_code == 200
    body = response.json()
    assert 0.0 <= body["anomaly_score"] <= 1.0
    assert body["model_version"].startswith("isoforest-")


def test_score_missing_required_field_returns_422() -> None:
    # account_id is required by ScoreRequest but omitted here.
    response = client.post(
        "/score",
        json={"instrument_id": "ACME", "window_features": {}},
    )
    assert response.status_code == 422


def test_score_empty_window_features_still_succeeds() -> None:
    # A caller with no tracked features yet (e.g. an account's very first
    # window) should get a scored response, not an error -- features.py's
    # zero-default handles this.
    response = client.post(
        "/score",
        json={"account_id": "ACC-1", "instrument_id": "ACME", "window_features": {}},
    )
    assert response.status_code == 200
    assert 0.0 <= response.json()["anomaly_score"] <= 1.0


def test_score_is_deterministic_across_requests() -> None:
    payload = {
        "account_id": "ACC-1",
        "instrument_id": "ACME",
        "window_features": {"order_count": 5000.0, "total_qty": 900000.0, "cancel_ratio": 0.5},
    }
    first = client.post("/score", json=payload).json()
    second = client.post("/score", json=payload).json()
    assert first == second


def test_score_unknown_feature_keys_are_ignored_not_rejected() -> None:
    response = client.post(
        "/score",
        json={
            "account_id": "ACC-1",
            "instrument_id": "ACME",
            "window_features": {"some_future_feature": 1.0},
        },
    )
    assert response.status_code == 200
