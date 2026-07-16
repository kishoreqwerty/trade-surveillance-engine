#include <gtest/gtest.h>

#include "db.hpp"

TEST(dbModule, ScaffoldPlaceholderPasses) {
    EXPECT_TRUE(tse::db::module_ready());
}
