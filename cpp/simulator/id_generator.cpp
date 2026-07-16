#include "id_generator.hpp"

#include <iomanip>
#include <sstream>

namespace tse::simulator {

IdGenerator::IdGenerator(std::string prefix, int64_t start)
    : prefix_(std::move(prefix)), counter_(start) {}

std::string IdGenerator::next() {
    std::ostringstream oss;
    oss << prefix_ << "-" << std::setw(6) << std::setfill('0') << counter_;
    ++counter_;
    return oss.str();
}

}  // namespace tse::simulator
