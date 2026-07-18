#include "ml_score_client.hpp"

// httplib.h is a large single-header library; keeping it out of
// ml_score_client.hpp (only forward-declaring httplib::Client there) means
// nothing that merely calls MlScoreClient — most of this codebase — pays
// for parsing it.
#include <httplib.h>

#include "ml_json.hpp"

namespace tse::ml_client {

namespace {
// httplib's timeout setters take (seconds, microseconds) separately, not
// a single millisecond count.
std::pair<time_t, time_t> to_sec_usec(int timeout_ms) {
    return {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
}
}  // namespace

MlScoreClient::MlScoreClient(MlScoreClientConfig config) : client_(std::make_unique<httplib::Client>(config.base_url)) {
    auto [connect_sec, connect_usec] = to_sec_usec(config.connect_timeout_ms);
    auto [read_sec, read_usec] = to_sec_usec(config.read_timeout_ms);
    client_->set_connection_timeout(connect_sec, connect_usec);
    client_->set_read_timeout(read_sec, read_usec);
    client_->set_write_timeout(read_sec, read_usec);
}

MlScoreClient::~MlScoreClient() = default;
MlScoreClient::MlScoreClient(MlScoreClient&&) noexcept = default;
MlScoreClient& MlScoreClient::operator=(MlScoreClient&&) noexcept = default;

std::optional<ScoringResult> MlScoreClient::score(const ScoringRequest& request) {
    const std::string body = encode_scoring_request(request);
    httplib::Result res = client_->Post("/score", body, "application/json");
    if (!res) return std::nullopt;  // connection refused, timed out, etc.
    if (res->status != 200) return std::nullopt;
    return decode_scoring_response(res->body);
}

}  // namespace tse::ml_client
