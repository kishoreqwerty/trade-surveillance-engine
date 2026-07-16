#include <gtest/gtest.h>

#include "ingestion.hpp"

TEST(ingestionModule, ScaffoldPlaceholderPasses) {
    EXPECT_TRUE(tse::ingestion::module_ready());
}
