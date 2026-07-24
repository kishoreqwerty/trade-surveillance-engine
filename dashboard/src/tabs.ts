export const TABS = ["MONITOR", "ALERTS", "BOOK", "EVALUATION"] as const;
export type Tab = (typeof TABS)[number];
