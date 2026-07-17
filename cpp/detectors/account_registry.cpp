#include "account_registry.hpp"

#include <algorithm>

namespace tse::detectors {

void AccountRegistry::add(Entity entity) {
    const std::string account_id = entity.account_id;
    entities_[account_id] = std::move(entity);
}

const Entity* AccountRegistry::lookup(const std::string& account_id) const {
    auto it = entities_.find(account_id);
    return it == entities_.end() ? nullptr : &it->second;
}

bool AccountRegistry::same_beneficial_owner(const std::string& account_a, const std::string& account_b) const {
    const Entity* a = lookup(account_a);
    const Entity* b = lookup(account_b);
    if (a == nullptr || b == nullptr) return false;
    if (a->beneficial_owner_id.empty() || b->beneficial_owner_id.empty()) return false;
    return a->beneficial_owner_id == b->beneficial_owner_id;
}

namespace {
bool names(const Entity* entity, const std::string& target) {
    if (entity == nullptr) return false;
    return std::find(entity->linked_account_ids.begin(), entity->linked_account_ids.end(), target) !=
           entity->linked_account_ids.end();
}
}  // namespace

bool AccountRegistry::are_linked(const std::string& account_a, const std::string& account_b) const {
    return names(lookup(account_a), account_b) || names(lookup(account_b), account_a);
}

bool AccountRegistry::is_related(const std::string& account_a, const std::string& account_b) const {
    if (account_a == account_b) return true;
    return same_beneficial_owner(account_a, account_b) || are_linked(account_a, account_b);
}

}  // namespace tse::detectors
