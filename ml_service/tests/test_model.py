from app.model import AnomalyModel

# Training happens once for this whole test file (fitting on 4000 synthetic
# samples is fast, but there's no reason to pay for it once per test).
_model = AnomalyModel()

_TYPICAL_BASELINE_WINDOW = {
    "order_count": 25.0,
    "total_qty": 3750.0,
    "avg_qty": 150.0,
    "cancel_ratio": 0.2,
    "orders_per_second": 25.0 / 60.0,
}

# Orders of magnitude beyond anything the training distribution could
# plausibly produce -- order_count's training lognormal(mean=3.2, sigma=0.6)
# puts even a +5-sigma sample under 500 (see training_data.py).
_EXTREME_OUTLIER_WINDOW = {
    "order_count": 50_000.0,
    "total_qty": 50_000_000.0,
    "avg_qty": 1_000.0,
    "cancel_ratio": 0.99,
    "orders_per_second": 50_000.0 / 60.0,
}


def test_model_version_is_stable_across_instances() -> None:
    # Two independently-constructed models, same training config -> same
    # version string (see model.py: it hashes config, not wall-clock
    # time).
    other_model = AnomalyModel()
    assert _model.model_version == other_model.model_version
    assert _model.model_version.startswith("isoforest-")


def test_score_is_always_within_unit_interval() -> None:
    samples = [{}, _TYPICAL_BASELINE_WINDOW, _EXTREME_OUTLIER_WINDOW]
    for window_features in samples:
        score = _model.score(window_features)
        assert 0.0 <= score <= 1.0


def test_typical_baseline_shaped_window_scores_low() -> None:
    score = _model.score(_TYPICAL_BASELINE_WINDOW)
    assert score < 0.5, f"expected a typical baseline window to score as non-anomalous, got {score}"


def test_extreme_outlier_window_scores_high() -> None:
    score = _model.score(_EXTREME_OUTLIER_WINDOW)
    assert score > 0.5, f"expected an extreme outlier window to score as anomalous, got {score}"


def test_extreme_outlier_scores_higher_than_typical_baseline() -> None:
    assert _model.score(_EXTREME_OUTLIER_WINDOW) > _model.score(_TYPICAL_BASELINE_WINDOW)


def test_scoring_is_deterministic_for_the_same_input() -> None:
    window_features = {
        "order_count": 5000.0,
        "total_qty": 900_000.0,
        "avg_qty": 180.0,
        "cancel_ratio": 0.5,
        "orders_per_second": 83.3,
    }
    assert _model.score(window_features) == _model.score(window_features)
