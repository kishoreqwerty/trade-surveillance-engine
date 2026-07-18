#pragma once

#include <memory>
#include <optional>
#include <string>

#include "scoring_request.hpp"

namespace httplib {
class Client;
}  // namespace httplib

namespace tse::ml_client {

struct MlScoreClientConfig {
    std::string base_url{"http://127.0.0.1:8000"};
    int connect_timeout_ms{200};
    int read_timeout_ms{200};
};

// Blocking HTTP client with a strict timeout, wrapping cpp-httplib. Only
// ever called from MlScoringWorker's own background thread (see
// ml_scoring_worker.hpp) — never from the hot consumer thread. That's
// what makes "async, off the hot path" (architecture doc §3) a structural
// property of this design, not a promise about how it happens to be used:
// nothing about this class prevents a caller from invoking it
// synchronously, but nothing in this codebase ever does.
class MlScoreClient {
public:
    explicit MlScoreClient(MlScoreClientConfig config = {});
    ~MlScoreClient();
    MlScoreClient(const MlScoreClient&) = delete;
    MlScoreClient& operator=(const MlScoreClient&) = delete;
    MlScoreClient(MlScoreClient&&) noexcept;
    MlScoreClient& operator=(MlScoreClient&&) noexcept;

    // Blocks for up to connect_timeout_ms + read_timeout_ms. Returns
    // std::nullopt for *any* failure — connection refused, timeout,
    // non-200 status, or a response that doesn't parse as the expected
    // shape. Never throws: "the ML service is unavailable right now" is
    // an ordinary, expected outcome for this method, not an exceptional
    // one — see MlScoringWorker for what happens next (nothing crashes,
    // nothing blocks further, the caller just gets no opinion for this
    // window).
    std::optional<ScoringResult> score(const ScoringRequest& request);

private:
    std::unique_ptr<httplib::Client> client_;
};

}  // namespace tse::ml_client
