"""Feature schema for the Isolation Forest anomaly model.

Volume/frequency features computed over a rolling window of one account's
order activity for one instrument -- exactly what the architecture doc's
ML microservice contract's `window_features` dict carries:

    POST /score
    { "account_id": ..., "instrument_id": ..., "window_features": {...} }

FEATURE_NAMES is the single source of truth for column order: requests
carry a dict (order-independent on the wire), but the model is trained on
a fixed-order numpy array, so every place that turns a dict into a vector
goes through features_to_vector() rather than assuming key order.
"""

FEATURE_NAMES = [
    "order_count",       # frequency: number of orders in the window
    "total_qty",         # volume: sum of order quantities in the window
    "avg_qty",           # volume: mean order size
    "cancel_ratio",      # frequency: cancelled orders / total orders, in [0, 1]
    "orders_per_second",  # frequency: order_count / window duration
]


def features_to_vector(window_features: dict[str, float]) -> list[float]:
    """Extracts FEATURE_NAMES, in order, from a request's window_features
    dict. A missing key defaults to 0.0 -- a caller that hasn't observed a
    given feature yet (e.g. no cancels in the window) is a legitimate
    zero, not a malformed request, so this never raises for partial
    dicts.
    """
    return [float(window_features.get(name, 0.0)) for name in FEATURE_NAMES]
