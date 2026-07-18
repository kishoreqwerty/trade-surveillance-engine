# ml_service/ — design decisions

Companion to `P2_trade_surveillance_engine_architecture.md`'s "ML
microservice contract" section. This is the narrative version: what the
model actually is, why the training data is what it is, and how the
graceful-degradation hook this service exposes gets used from the C++
side (`cpp/ml_client/`).

## Independence, by construction

Per CLAUDE.md: "Keep `ml_service/` and `dashboard/` fully independent — no
shared build tooling, no implicit coupling beyond their documented REST
contracts." This service has its own `.venv`, its own `requirements.txt`,
and its own pytest suite that never imports anything from `cpp/`. Its
synthetic training data generator (`app/training_data.py`) is
deliberately a *separate*, self-contained numpy generator, not a caller
into `cpp/simulator/` — there is no way to reach Phase 1's C++ generator
from Python without shared build tooling, which is exactly what the
independence rule rules out.

## Feature schema and training data

Five volume/frequency features (`app/features.py`): `order_count`,
`total_qty`, `avg_qty`, `cancel_ratio`, `orders_per_second`. Training data
(`app/training_data.py`) is a small numpy generator producing plausible
*baseline* (non-abusive) window feature vectors — lognormal order counts
and sizes, a beta-distributed cancel ratio skewed low, `total_qty` and
`orders_per_second` derived consistently from `order_count` rather than
sampled independently (real window aggregates co-vary; training on
features that don't would teach the model a trivially-easy-to-look-
anomalous baseline). This is calibration-adjacent but not the same thing
as `cpp/calibration/`'s WRDS/TAQ work — it's a good-enough synthetic
baseline for `IsolationForest` to learn a shape from, not a claim of
statistical realism.

The model (`app/model.py`) trains fresh at every process startup — 4,000
synthetic samples, well under a second — rather than loading a checked-in
serialized model file. A fresh-trained model can never silently drift out
of sync with `FEATURE_NAMES` or the generator; a stale file could.
`model_version` is a hash of the training configuration (seed, sample
count, feature names), not a timestamp — two processes started from the
same code produce the same version string, which is what actually lets a
caller distinguish "this was retrained with different config" from "this
is just a different process instance."

`IsolationForest.decision_function()` returns positive for inliers,
negative for outliers, roughly centered on zero with no fixed bound.
`anomaly_score` maps that through a logistic squash (not a hard clip) —
clipping would flatten any sufficiently extreme input straight to exactly
0.0 or 1.0, losing the relative ordering a future threshold sweep would
want to sweep over. Verified empirically, not just asserted: a
"typical baseline" window (drawn from the same distribution family
training used) scores below 0.5; an outlier several orders of magnitude
beyond anything the training distribution could plausibly produce scores
above 0.5 and strictly higher than the typical case
(`tests/test_model.py`).

## The `ML_SERVICE_ARTIFICIAL_DELAY_MS` hook

`app/main.py`'s `/score` handler reads this environment variable once per
request and, if set and positive, sleeps that many milliseconds before
scoring. This is the *only* thing it changes — normal operation reads an
unset env var and pays a negligible `os.environ.get()` per request. It
exists specifically so Phase 7's graceful-degradation proof
(`cpp/tests/ml_client/graceful_degradation_test.cpp`) can launch a real
copy of this service and make it artificially slow, without needing a
second code path to keep in sync with the real one. Proven to actually
delay responses by the configured amount, not just declared, in
`tests/test_artificial_delay.py`.

## Verification

22 pytest tests across five files: feature extraction (`test_features.py`),
synthetic training data shape/range/determinism (`test_training_data.py`),
the model's actual discrimination behavior — typical-scores-low,
outlier-scores-high, deterministic, always in `[0,1]`
(`test_model.py`), the `/score` and `/health` endpoints including
malformed-request handling (`test_main.py`), and the artificial-delay hook
(`test_artificial_delay.py`). All passing, run independent of the C++
pipeline — `.venv/bin/python -m pytest tests/` from this directory.
