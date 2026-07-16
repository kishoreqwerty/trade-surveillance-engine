#include <gtest/gtest.h>

#include "simulator.hpp"

TEST(simulatorModule, ScaffoldPlaceholderPasses) {
    EXPECT_TRUE(tse::simulator::module_ready());
}
