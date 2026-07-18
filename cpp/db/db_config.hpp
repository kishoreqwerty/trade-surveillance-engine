#pragma once

#include <string>

namespace tse::db {

// Defaults match docker-compose.yml's timescaledb service exactly -- a
// local, unauthenticated dev Postgres instance, the same posture this
// project already takes with its Kafka setup (cpp/ingestion/README.md).
struct DbConfig {
    std::string host{"127.0.0.1"};
    int port{5432};
    std::string dbname{"trade_surveillance"};
    std::string user{"tse"};
    std::string password{"tse_dev_password"};

    std::string connection_string() const;
};

}  // namespace tse::db
