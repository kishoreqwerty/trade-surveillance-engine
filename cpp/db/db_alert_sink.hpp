#pragma once

#include "alert_sink.hpp"
#include "alert_store.hpp"

namespace tse::db {

// The real TimescaleDB-backed IAlertSink implementation pipeline/'s own
// header comment (alert_sink.hpp) has been forward-referring to since
// Phase 6: "Phase 8 (db/) will provide the real ... implementation."
//
// Writes synchronously, on whatever thread calls on_alert() -- deliberately
// not async. Phase 8's scope is the store, schema, and query layer, proven
// against a real database; there is no live production entrypoint yet that
// would put this on LiveConsumer's hot thread (that's a Phase 9+ wiring
// decision). If and when one exists, the async-off-the-hot-path pattern
// ml_client/'s MlScoringWorker already established (bounded queue +
// background writer thread) is the template to reuse -- not something to
// build speculatively here before an actual caller needs it.
class DbAlertSink : public tse::pipeline::IAlertSink {
public:
    explicit DbAlertSink(AlertStore* store) : store_(store) {}

    void on_alert(const tse::detectors::Alert& alert) override { store_->insert_alert(alert); }

private:
    AlertStore* store_;
};

}  // namespace tse::db
