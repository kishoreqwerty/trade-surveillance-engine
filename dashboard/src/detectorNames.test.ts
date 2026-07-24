import { test } from "node:test";
import assert from "node:assert/strict";
import { DETECTOR_NAMES } from "./detectorNames.ts";

// Mirrors the six detectors cpp/pipeline/'s LivePipeline actually
// registers (cpp/api/main.cpp's make_six_detectors(),
// cpp/harness/replay_runner.cpp's make_five_detectors() + MlAnomalyDetector)
// -- kept as an independent, hand-written list here rather than importing
// DETECTOR_NAMES and comparing it to itself, which would pass trivially
// no matter what the constant said.
const REAL_DETECTORS = [
  "WashTradeDetector",
  "SpoofingLayeringDetector",
  "MarkingTheCloseDetector",
  "FrontRunningDetector",
  "StatisticalBaselineDetector",
  "MlAnomalyDetector",
];

test("DETECTOR_NAMES contains exactly the six real detectors, no more, no fewer", () => {
  assert.deepEqual([...DETECTOR_NAMES].sort(), [...REAL_DETECTORS].sort());
});

test("DETECTOR_NAMES has no duplicate entries", () => {
  assert.equal(new Set(DETECTOR_NAMES).size, DETECTOR_NAMES.length);
});
