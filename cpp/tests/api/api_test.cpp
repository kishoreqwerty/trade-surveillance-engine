#include <gtest/gtest.h>

#include "api.hpp"

TEST(apiModule, ScaffoldPlaceholderPasses) {
    EXPECT_TRUE(tse::api::module_ready());
}
