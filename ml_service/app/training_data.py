"""Synthetic baseline (non-abusive) training data for the Isolation Forest.

Per CLAUDE.md's data rules, no real market data is ever used here, and
WRDS/TAQ (calibration-only, cpp/calibration/) never touches this service.
This generator is deliberately independent of cpp/simulator/ too -- Phase
1's C++ generator isn't reachable from Python without shared build
tooling, which CLAUDE.md explicitly rules out for ml_service/ ("no shared
build tooling, no implicit coupling beyond their documented REST
contracts"). This is a small, self-contained numpy generator producing
plausible "normal" volume/frequency window feature vectors -- good enough
to give IsolationForest a baseline distribution to learn from, not a
claim of calibrated realism (that calibration work, if ever wanted here,
belongs in cpp/calibration/, not this file).
"""

import numpy as np

from .features import FEATURE_NAMES

WINDOW_DURATION_SECONDS = 60.0


def generate_baseline_features(n_samples: int, rng: np.random.Generator) -> np.ndarray:
    """Returns an (n_samples, len(FEATURE_NAMES)) array of synthetic
    baseline (non-abusive) window feature vectors, column order matching
    FEATURE_NAMES exactly.
    """
    order_count = rng.lognormal(mean=3.2, sigma=0.6, size=n_samples)  # ~25 orders/window, typical
    avg_qty = rng.lognormal(mean=5.0, sigma=0.5, size=n_samples)  # ~150 shares/order, typical
    # total_qty tracks order_count * avg_qty with a little independent
    # noise, rather than being drawn fully independently -- real window
    # aggregates are internally consistent this way, and an
    # IsolationForest trained on features that don't co-vary realistically
    # would learn a baseline that's trivially easy to look "anomalous"
    # against just by being self-consistent.
    total_qty = order_count * avg_qty * rng.normal(loc=1.0, scale=0.05, size=n_samples)
    cancel_ratio = np.clip(rng.beta(a=2.0, b=8.0, size=n_samples), 0.0, 1.0)  # skewed low, occasional higher
    orders_per_second = order_count / WINDOW_DURATION_SECONDS

    features = np.column_stack([order_count, total_qty, avg_qty, cancel_ratio, orders_per_second])
    assert features.shape == (n_samples, len(FEATURE_NAMES))
    return features
