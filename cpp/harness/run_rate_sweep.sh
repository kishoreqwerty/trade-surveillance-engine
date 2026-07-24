#!/bin/bash
# Regenerates cpp/harness/results/spoofing_rate_sweep.json -- the real,
# rerunnable mechanism behind calibration/PARAMETER_MAPPING.md's
# "Relative precision collapse ... 168.5x before -> 53.3x after" finding,
# so that number is never hand-transcribed a second time (see main.cpp's
# TSE_RATE_OVERRIDE comment).
#
# Runs tse_harness_eval once per documented rate point (TSE_RATE_OVERRIDE
# set each time), each producing its own --json snapshot, then aggregates
# SpoofingLayeringDetector's precision at threshold=0.5 from each into one
# combined JSON. Must be run from the repo root (matches every other
# manual-run entrypoint in this project -- docker compose, the demo
# server, etc.).
set -euo pipefail

HARNESS_BIN="./build-bench/cpp/harness/tse_harness_eval"
RESULTS_DIR="cpp/harness/results"
TMP_DIR=$(mktemp -d)
trap 'rm -rf "$TMP_DIR"' EXIT

if [ ! -x "$HARNESS_BIN" ]; then
    echo "FATAL: $HARNESS_BIN not found or not executable -- build tse_harness_eval first." >&2
    exit 1
fi

mkdir -p "$RESULTS_DIR"

# The exact 11 points already documented in calibration/PARAMETER_MAPPING.md
# -- the last one (250.35) is main_eval_config()'s own natural, unmodified
# rate (16.69 * 15 instruments), not an arbitrary sweep endpoint.
RATES=(3 5 7 9 12 16 20 25 75 150 250.35)

echo "Running ${#RATES[@]}-point rate sweep against $HARNESS_BIN ..."
for rate in "${RATES[@]}"; do
    echo "  rate=$rate ..."
    TSE_RATE_OVERRIDE="$rate" "$HARNESS_BIN" --json "$TMP_DIR/rate_$rate.json" > "$TMP_DIR/rate_$rate.log" 2>&1
done

python3 - "$RESULTS_DIR/spoofing_rate_sweep.json" "$TMP_DIR" "${RATES[@]}" << 'PYEOF'
import json
import sys
import time

out_path = sys.argv[1]
tmp_dir = sys.argv[2]
rates = sys.argv[3:]

points = []
for rate in rates:
    with open(f"{tmp_dir}/rate_{rate}.json") as f:
        data = json.load(f)
    entry = next(d for d in data["detectors"] if d["name"] == "SpoofingLayeringDetector")
    points.append({
        "rate_per_sec": float(rate),
        "threshold": entry["threshold"],
        "tp": entry["tp"],
        "fp": entry["fp"],
        "precision": entry["precision"],
        "recall": entry["recall"],
    })

snapshot = {
    "generated_at_unix_ns": time.time_ns(),
    "detector": "SpoofingLayeringDetector",
    "points": points,
}
with open(out_path, "w") as f:
    json.dump(snapshot, f, indent=2)

print(f"Wrote {out_path} ({len(points)} points)")
PYEOF
