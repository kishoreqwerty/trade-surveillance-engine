#pragma once

#include <mutex>
#include <vector>

#include "alert.hpp"

namespace tse::pipeline {

// Where LiveConsumer forwards Alerts produced by LivePipeline::process().
// Phase 8 (db/) will provide the real TimescaleDB-backed implementation;
// this phase only needs the plumbing to exist and be observable, per the
// build guide's "Done when: ... simulated FIX flow to live alerts" — it
// doesn't call for persistence, which is explicitly Phase 8's job.
class IAlertSink {
public:
    virtual ~IAlertSink() = default;
    virtual void on_alert(const tse::detectors::Alert& alert) = 0;
};

// Simple in-memory collector for tests and this phase's demonstration —
// not a production sink. Internally synchronized even though the current
// design has exactly one consumer thread calling on_alert() (mirrors
// LivePipeline's single-consumer contract) — cheap insurance for a class
// whose only job is "don't lose an alert," and callers may legitimately
// read alerts() from a different thread than the one producing them (e.g.
// a test thread inspecting results after joining the consumer).
class CollectingAlertSink : public IAlertSink {
public:
    void on_alert(const tse::detectors::Alert& alert) override {
        std::lock_guard<std::mutex> lock(mutex_);
        alerts_.push_back(alert);
    }

    std::vector<tse::detectors::Alert> alerts() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return alerts_;
    }

private:
    mutable std::mutex mutex_;
    std::vector<tse::detectors::Alert> alerts_;
};

}  // namespace tse::pipeline
