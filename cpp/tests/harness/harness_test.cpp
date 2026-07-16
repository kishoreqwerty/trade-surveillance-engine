#include <gtest/gtest.h>

#include "harness.hpp"

TEST(harnessModule, ScaffoldPlaceholderPasses) {
    EXPECT_TRUE(tse::harness::module_ready());
}
