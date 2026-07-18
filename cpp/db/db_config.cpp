#include "db_config.hpp"

namespace tse::db {

std::string DbConfig::connection_string() const {
    return "host=" + host + " port=" + std::to_string(port) + " dbname=" + dbname + " user=" + user +
           " password=" + password;
}

}  // namespace tse::db
