#include "account_registry.hpp"

#include <gtest/gtest.h>

using tse::detectors::AccountRegistry;
using tse::detectors::Entity;

TEST(AccountRegistry, LookupReturnsNullptrForUnknownAccount) {
    AccountRegistry registry;
    EXPECT_EQ(registry.lookup("NEVER_ADDED"), nullptr);
}

TEST(AccountRegistry, LookupReturnsRegisteredEntity) {
    AccountRegistry registry;
    registry.add(Entity{"ACC-1", "OWNER-A", "client", {}});
    const Entity* found = registry.lookup("ACC-1");
    ASSERT_NE(found, nullptr);
    EXPECT_EQ(found->beneficial_owner_id, "OWNER-A");
}

TEST(AccountRegistry, SameBeneficialOwnerMatches) {
    AccountRegistry registry;
    registry.add(Entity{"ACC-1", "OWNER-A", "client", {}});
    registry.add(Entity{"ACC-2", "OWNER-A", "client", {}});
    EXPECT_TRUE(registry.same_beneficial_owner("ACC-1", "ACC-2"));
    EXPECT_TRUE(registry.same_beneficial_owner("ACC-2", "ACC-1"));  // symmetric
}

TEST(AccountRegistry, DifferentBeneficialOwnerDoesNotMatch) {
    AccountRegistry registry;
    registry.add(Entity{"ACC-1", "OWNER-A", "client", {}});
    registry.add(Entity{"ACC-2", "OWNER-B", "client", {}});
    EXPECT_FALSE(registry.same_beneficial_owner("ACC-1", "ACC-2"));
}

TEST(AccountRegistry, EmptyBeneficialOwnerNeverMatchesAnotherEmptyOne) {
    AccountRegistry registry;
    registry.add(Entity{"ACC-1", "", "client", {}});
    registry.add(Entity{"ACC-2", "", "client", {}});
    EXPECT_FALSE(registry.same_beneficial_owner("ACC-1", "ACC-2"));
}

TEST(AccountRegistry, SameBeneficialOwnerFalseForUnregisteredAccount) {
    AccountRegistry registry;
    registry.add(Entity{"ACC-1", "OWNER-A", "client", {}});
    EXPECT_FALSE(registry.same_beneficial_owner("ACC-1", "NEVER_ADDED"));
}

TEST(AccountRegistry, AreLinkedMatchesOneDirectionalDeclaration) {
    AccountRegistry registry;
    registry.add(Entity{"ACC-1", "OWNER-A", "client", {"ACC-2"}});
    registry.add(Entity{"ACC-2", "OWNER-B", "client", {}});  // doesn't declare the link back
    EXPECT_TRUE(registry.are_linked("ACC-1", "ACC-2"));
    EXPECT_TRUE(registry.are_linked("ACC-2", "ACC-1"));  // still symmetric from the caller's point of view
}

TEST(AccountRegistry, UnlinkedAccountsAreNotLinked) {
    AccountRegistry registry;
    registry.add(Entity{"ACC-1", "OWNER-A", "client", {}});
    registry.add(Entity{"ACC-2", "OWNER-B", "client", {}});
    EXPECT_FALSE(registry.are_linked("ACC-1", "ACC-2"));
}

TEST(AccountRegistry, IsRelatedTrueForIdenticalAccountEvenIfUnregistered) {
    AccountRegistry registry;
    EXPECT_TRUE(registry.is_related("ACC-1", "ACC-1"));
}

TEST(AccountRegistry, IsRelatedTrueViaBeneficialOwnerOrLink) {
    AccountRegistry registry;
    registry.add(Entity{"ACC-1", "OWNER-A", "client", {}});
    registry.add(Entity{"ACC-2", "OWNER-A", "client", {}});
    registry.add(Entity{"ACC-3", "OWNER-C", "client", {"ACC-4"}});
    registry.add(Entity{"ACC-4", "OWNER-D", "client", {}});

    EXPECT_TRUE(registry.is_related("ACC-1", "ACC-2"));  // beneficial owner
    EXPECT_TRUE(registry.is_related("ACC-3", "ACC-4"));  // explicit link
}

TEST(AccountRegistry, IsRelatedFalseForGenuinelyUnrelatedAccounts) {
    AccountRegistry registry;
    registry.add(Entity{"ACC-1", "OWNER-A", "client", {}});
    registry.add(Entity{"ACC-2", "OWNER-B", "client", {}});
    EXPECT_FALSE(registry.is_related("ACC-1", "ACC-2"));
}
