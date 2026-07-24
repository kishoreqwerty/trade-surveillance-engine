// The fixed set of detectors this system can ever emit an alert from --
// see cpp/pipeline/'s LivePipeline wiring (cpp/api/main.cpp's
// make_six_detectors(), cpp/harness/replay_runner.cpp's
// make_five_detectors() + MlAnomalyDetector) -- six detectors total,
// never a runtime-variable set. Kept in its own JSX-free module (not
// inline in AlertsTab.tsx) so it's importable from a plain Node test
// without pulling in React/JSX.
//
// AlertsTab.tsx's detector-filter dropdown is built from this literal
// list, not from any alerts sample (a recent-N poll or the currently
// loaded page) -- a detector that fires rarely enough to be absent from
// whichever sample is being looked at (MarkingTheCloseDetector's own
// per-account-group lifetime dedup is exactly this case) would then be
// visibly present in the alert table but missing from its own filter --
// a real bug found in production, not a hypothetical. See
// detectorNames.test.ts for the regression coverage.
export const DETECTOR_NAMES = [
  "FrontRunningDetector",
  "MarkingTheCloseDetector",
  "MlAnomalyDetector",
  "SpoofingLayeringDetector",
  "StatisticalBaselineDetector",
  "WashTradeDetector",
];
