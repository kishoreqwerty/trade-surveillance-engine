import asyncio
import os

from fastapi import FastAPI

from .model import AnomalyModel
from .schemas import ScoreRequest, ScoreResponse

app = FastAPI(title="trade-surveillance-ml-service")

# Trained once at process startup -- see model.py's docstring for why this
# isn't loaded from a checked-in file instead.
_model = AnomalyModel()


@app.get("/health")
def health() -> dict:
    return {"status": "ok"}


@app.post("/score", response_model=ScoreResponse)
async def score(request: ScoreRequest) -> ScoreResponse:
    # Test-only hook for Phase 7's graceful-degradation proof (see
    # ml_service/README.md and cpp/ml_client/README.md): setting
    # ML_SERVICE_ARTIFICIAL_DELAY_MS makes every /score response take at
    # least that many milliseconds, simulating a slow/overloaded service
    # without a second code path to keep in sync with this one. Unset or
    # 0 in normal operation -- reading one env var per request is the only
    # cost when this feature isn't in use.
    artificial_delay_ms = float(os.environ.get("ML_SERVICE_ARTIFICIAL_DELAY_MS", "0"))
    if artificial_delay_ms > 0:
        await asyncio.sleep(artificial_delay_ms / 1000.0)

    anomaly_score = _model.score(request.window_features)
    return ScoreResponse(anomaly_score=anomaly_score, model_version=_model.model_version)
