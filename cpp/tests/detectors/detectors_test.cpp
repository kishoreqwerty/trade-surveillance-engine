#include <gtest/gtest.h>

#include "detectors.hpp"

TEST(detectorsModule, ScaffoldPlaceholderPasses) {
    EXPECT_TRUE(tse::detectors::module_ready());
}
