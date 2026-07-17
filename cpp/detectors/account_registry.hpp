#pragma once

#include <string>
#include <unordered_map>
#include <vector>

namespace tse::detectors {

// Live-mode account/entity metadata: account_id, beneficial_owner_id,
// entity_type, linked_account_ids — mirrors the architecture doc's
// Account/Entity model exactly (see also cpp/simulator/types.hpp's Account,
// the evaluation-mode struct with the same fields). This is deliberately a
// separate type, not a shared one: detectors/ depends only on orderbook/
// per the architecture doc's dependency table, and simulator:: is a
// generator-only module — cpp/simulator/account_registry.hpp's
// AccountRegistry is built for *random account assignment during synthetic
// generation*, a fundamentally different job from the fast by-account-id
// lookup this class exists for. `entity_type` is carried for schema
// fidelity with the documented data model even though no detector in this
// phase branches on its value.
struct Entity {
    std::string account_id;
    std::string beneficial_owner_id;
    std::string entity_type;
    std::vector<std::string> linked_account_ids;
};

// Read-heavy lookup registry over Entity records, used by every detector
// that needs to reason about account relationships (WashTradeDetector:
// same-beneficial-owner/linked matching; FrontRunningDetector:
// related-account sequencing). Never throws — detectors call into this on
// their hot path and CLAUDE.md bars exceptions across the IDetector
// boundary, so an unregistered account_id is treated as "nothing known
// about it," not an error.
class AccountRegistry {
public:
    void add(Entity entity);

    // nullptr if account_id isn't registered.
    const Entity* lookup(const std::string& account_id) const;

    // True iff both accounts are registered, both have a non-empty
    // beneficial_owner_id, and those owner IDs match. An empty
    // beneficial_owner_id never matches another empty one — "unknown
    // owner" isn't itself a relationship, it's a conservative default that
    // avoids manufacturing false positives out of missing data.
    bool same_beneficial_owner(const std::string& account_a, const std::string& account_b) const;

    // True iff either account's linked_account_ids names the other —
    // checked in both directions so the relation behaves symmetrically
    // even if the underlying data only declared it one way.
    bool are_linked(const std::string& account_a, const std::string& account_b) const;

    // The single relation detectors actually care about: identical
    // accounts (the plainest form of self-dealing), same beneficial owner,
    // or an explicit link. account_a == account_b is always related,
    // independent of whether either is even registered — self-trading
    // doesn't need registry data to be meaningful.
    bool is_related(const std::string& account_a, const std::string& account_b) const;

private:
    std::unordered_map<std::string, Entity> entities_;
};

}  // namespace tse::detectors
