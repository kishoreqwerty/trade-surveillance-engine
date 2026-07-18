"""Isolation Forest wrapper: trains on synthetic baseline volume/frequency
features at process startup and scores incoming window_features dicts.

Trained fresh at startup rather than loaded from a checked-in model
file: fitting IsolationForest on a few thousand synthetic samples takes
well under a second, and training fresh means the model a process serves
can never silently drift out of sync with FEATURE_NAMES or
training_data.py's generator the way a stale serialized model file could.
"""

from __future__ import annotations

import hashlib

import numpy as np
from sklearn.ensemble import IsolationForest

from .features import FEATURE_NAMES, features_to_vector
from .training_data import generate_baseline_features

N_TRAINING_SAMPLES = 4000
RANDOM_SEED = 42
# Steepness of the logistic squash in score() -- see that method's
# docstring. Larger = anomaly_score saturates toward 0/1 faster as
# decision_function moves away from its own zero point.
SQUASH_STEEPNESS = 4.0


class AnomalyModel:
    def __init__(self) -> None:
        rng = np.random.default_rng(RANDOM_SEED)
        training_features = generate_baseline_features(N_TRAINING_SAMPLES, rng)
        self._forest = IsolationForest(
            n_estimators=100,
            contamination="auto",
            random_state=RANDOM_SEED,
        )
        self._forest.fit(training_features)
        # A stable, reproducible version string: hashes the exact training
        # configuration, not a wall-clock timestamp. Two processes started
        # from the same code produce the same model_version -- what
        # actually lets a caller tell "this was retrained with different
        # data/config" apart from "this is just a different process
        # instance."
        version_input = f"{RANDOM_SEED}:{N_TRAINING_SAMPLES}:{FEATURE_NAMES}".encode()
        self.model_version = "isoforest-" + hashlib.sha256(version_input).hexdigest()[:12]

    def score(self, window_features: dict[str, float]) -> float:
        """Returns an anomaly score in [0, 1], higher = more anomalous.

        sklearn's IsolationForest.decision_function() returns positive
        values for inliers and negative for outliers, roughly centered on
        0, with no fixed bound on magnitude. Mapped through a logistic
        squash (not a hard clip) centered at 0: a hard clip would flatten
        any sufficiently extreme input straight to 0.0 or 1.0, throwing
        away the relative ordering a future threshold sweep (Phase 10-
        style, if this service is ever evaluated the same way) would want
        to sweep over.
        """
        vector = np.array([features_to_vector(window_features)])
        decision = float(self._forest.decision_function(vector)[0])
        anomaly_score = 1.0 / (1.0 + np.exp(SQUASH_STEEPNESS * decision))
        return float(np.clip(anomaly_score, 0.0, 1.0))
