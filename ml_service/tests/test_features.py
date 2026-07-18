from app.features import FEATURE_NAMES, features_to_vector


def test_features_to_vector_preserves_feature_name_order() -> None:
    window_features = {name: float(i) for i, name in enumerate(FEATURE_NAMES)}
    vector = features_to_vector(window_features)
    assert vector == [float(i) for i in range(len(FEATURE_NAMES))]


def test_features_to_vector_defaults_missing_keys_to_zero() -> None:
    # Only the first feature is provided -- the rest must default to 0.0,
    # not raise, since a caller not yet tracking every feature is normal.
    window_features = {FEATURE_NAMES[0]: 42.0}
    vector = features_to_vector(window_features)
    assert vector[0] == 42.0
    assert vector[1:] == [0.0] * (len(FEATURE_NAMES) - 1)


def test_features_to_vector_ignores_unknown_keys() -> None:
    window_features = {name: 1.0 for name in FEATURE_NAMES}
    window_features["some_future_feature_not_in_the_schema"] = 999.0
    vector = features_to_vector(window_features)
    assert vector == [1.0] * len(FEATURE_NAMES)
