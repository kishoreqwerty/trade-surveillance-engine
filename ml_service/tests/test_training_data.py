import numpy as np

from app.features import FEATURE_NAMES
from app.training_data import generate_baseline_features


def test_shape_matches_feature_names() -> None:
    rng = np.random.default_rng(1)
    features = generate_baseline_features(500, rng)
    assert features.shape == (500, len(FEATURE_NAMES))


def test_values_are_within_expected_ranges() -> None:
    rng = np.random.default_rng(1)
    features = generate_baseline_features(2000, rng)
    order_count = features[:, FEATURE_NAMES.index("order_count")]
    total_qty = features[:, FEATURE_NAMES.index("total_qty")]
    avg_qty = features[:, FEATURE_NAMES.index("avg_qty")]
    cancel_ratio = features[:, FEATURE_NAMES.index("cancel_ratio")]
    orders_per_second = features[:, FEATURE_NAMES.index("orders_per_second")]

    assert (order_count > 0).all()
    assert (total_qty > 0).all()
    assert (avg_qty > 0).all()
    assert (cancel_ratio >= 0.0).all() and (cancel_ratio <= 1.0).all()
    assert (orders_per_second > 0).all()
    # orders_per_second is a deterministic function of order_count (see
    # training_data.py) -- proves the two features are actually derived
    # consistently, not independently sampled in a way that could produce
    # self-contradictory synthetic rows.
    np.testing.assert_allclose(orders_per_second, order_count / 60.0)


def test_deterministic_given_seeded_rng() -> None:
    features_a = generate_baseline_features(100, np.random.default_rng(7))
    features_b = generate_baseline_features(100, np.random.default_rng(7))
    np.testing.assert_array_equal(features_a, features_b)


def test_different_seeds_produce_different_data() -> None:
    features_a = generate_baseline_features(100, np.random.default_rng(1))
    features_b = generate_baseline_features(100, np.random.default_rng(2))
    assert not np.array_equal(features_a, features_b)
