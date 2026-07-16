#include <gtest/gtest.h>

#include "orderbook.hpp"

TEST(orderbookModule, ScaffoldPlaceholderPasses) {
    EXPECT_TRUE(tse::orderbook::module_ready());
}
