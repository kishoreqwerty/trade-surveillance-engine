#include "account_registry.hpp"

#include <stdexcept>

#include "id_generator.hpp"

namespace tse::simulator {

AccountRegistry::AccountRegistry(const AccountRegistryConfig& config, std::mt19937_64& /*rng*/) {
    IdGenerator account_id_gen("ACC");
    IdGenerator owner_id_gen("OWN");

    for (int i = 0; i < config.num_independent_accounts; ++i) {
        Account account;
        account.account_id = account_id_gen.next();
        account.beneficial_owner_id = owner_id_gen.next();
        account.entity_type = EntityType::kClient;
        independent_indices_.push_back(accounts_.size());
        accounts_.push_back(std::move(account));
    }

    for (int i = 0; i < config.num_linked_pairs; ++i) {
        std::string owner_id = owner_id_gen.next();

        Account client;
        client.account_id = account_id_gen.next();
        client.beneficial_owner_id = owner_id;
        client.entity_type = EntityType::kClient;

        Account related;
        related.account_id = account_id_gen.next();
        related.beneficial_owner_id = owner_id;
        related.entity_type = EntityType::kProprietary;

        client.linked_account_ids.push_back(related.account_id);
        related.linked_account_ids.push_back(client.account_id);

        size_t client_index = accounts_.size();
        accounts_.push_back(std::move(client));
        size_t related_index = accounts_.size();
        accounts_.push_back(std::move(related));

        linked_pair_indices_.emplace_back(client_index, related_index);
    }
}

const Account& AccountRegistry::random_independent(std::mt19937_64& rng) const {
    if (independent_indices_.empty()) {
        throw std::logic_error("AccountRegistry: no independent accounts configured");
    }
    std::uniform_int_distribution<size_t> dist(0, independent_indices_.size() - 1);
    return accounts_[independent_indices_[dist(rng)]];
}

std::pair<const Account&, const Account&> AccountRegistry::random_linked_pair(
    std::mt19937_64& rng) const {
    if (linked_pair_indices_.empty()) {
        throw std::logic_error("AccountRegistry: no linked account pairs configured");
    }
    std::uniform_int_distribution<size_t> dist(0, linked_pair_indices_.size() - 1);
    const auto& [first_idx, second_idx] = linked_pair_indices_[dist(rng)];
    return {accounts_[first_idx], accounts_[second_idx]};
}

}  // namespace tse::simulator
