#pragma once

#include <random>
#include <utility>
#include <vector>

#include "types.hpp"

namespace tse::simulator {

struct AccountRegistryConfig {
    int num_independent_accounts{40};
    // Pairs of accounts sharing a beneficial owner, used by the wash-trade
    // and front-running injectors.
    int num_linked_pairs{8};
};

class AccountRegistry {
public:
    AccountRegistry(const AccountRegistryConfig& config, std::mt19937_64& rng);

    const std::vector<Account>& all() const { return accounts_; }
    const Account& random_independent(std::mt19937_64& rng) const;
    std::pair<const Account&, const Account&> random_linked_pair(std::mt19937_64& rng) const;

private:
    std::vector<Account> accounts_;
    std::vector<size_t> independent_indices_;
    std::vector<std::pair<size_t, size_t>> linked_pair_indices_;
};

}  // namespace tse::simulator
