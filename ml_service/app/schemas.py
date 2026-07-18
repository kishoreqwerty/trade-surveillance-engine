"""Pydantic request/response models for the /score contract documented in
P2_trade_surveillance_engine_architecture.md §3 ("ML microservice
contract").
"""

from pydantic import BaseModel, ConfigDict, Field


class ScoreRequest(BaseModel):
    account_id: str
    instrument_id: str
    window_features: dict[str, float] = Field(default_factory=dict)


class ScoreResponse(BaseModel):
    # "model_version" is the field name the architecture doc's contract
    # specifies -- pydantic v2 otherwise warns that any "model_*" field
    # looks like it collides with its own reserved namespace (model_dump()
    # etc.); this field isn't one of those, so the check is disabled here
    # rather than renaming a field an external contract dictates.
    model_config = ConfigDict(protected_namespaces=())

    anomaly_score: float
    model_version: str
