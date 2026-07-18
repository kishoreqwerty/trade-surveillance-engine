// Maps Alert.score (continuous, [0,1] by convention -- see
// cpp/detectors/alert.hpp) onto the dataviz skill's fixed, reserved status
// palette. Status colors are never used for anything else in this app (no
// "series 4" reuse) and always ship with an icon + text label, never color
// alone, per the skill's status-palette rule.
export interface SeverityInfo {
  label: string;
  color: string;
  icon: string;
}

export function severityFor(score: number): SeverityInfo {
  if (score >= 0.85) return { label: "Critical", color: "var(--status-critical)", icon: "▲" };
  if (score >= 0.65) return { label: "Serious", color: "var(--status-serious)", icon: "▲" };
  if (score >= 0.4) return { label: "Warning", color: "var(--status-warning)", icon: "▲" };
  return { label: "Low", color: "var(--status-good)", icon: "●" };
}

// Fixed categorical hue order (never cycled/reassigned per instance) --
// detector names are hashed into the 8-slot categorical palette
// consistently across the whole app, so the same detector always reads as
// the same color in both the alert queue and the event timeline.
const CATEGORICAL_SLOTS = [
  "var(--series-1)",
  "var(--series-2)",
  "var(--series-3)",
  "var(--series-4)",
  "var(--series-5)",
  "var(--series-6)",
  "var(--series-7)",
  "var(--series-8)",
];

const detectorSlot = new Map<string, string>();
let nextSlot = 0;

export function colorForDetector(detectorName: string): string {
  let color = detectorSlot.get(detectorName);
  if (!color) {
    color = CATEGORICAL_SLOTS[nextSlot % CATEGORICAL_SLOTS.length];
    nextSlot += 1;
    detectorSlot.set(detectorName, color);
  }
  return color;
}
