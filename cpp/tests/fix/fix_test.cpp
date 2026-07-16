#include <gtest/gtest.h>

#include "fix.hpp"

TEST(fixModule, ScaffoldPlaceholderPasses) {
    EXPECT_TRUE(tse::fix::module_ready());
}
