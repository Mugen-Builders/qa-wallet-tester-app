// Runtime configuration (addresses, sizes). Read from environment variables by the drivers and
// by tests; the engine never reads the environment itself.
#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace qawallet {

struct Config {
    // Devnet defaults come from `cartesi address-book` (rollups-contracts v3.0.0-alpha.10).
    Address token;                 // the single withdrawable ERC-20 (accounts drive)
    Address ether_portal;
    Address erc20_portal;
    Address erc721_portal;
    Address erc1155_single_portal;
    Address erc1155_batch_portal;
    bool log{true};                // stderr logging

    // State layout (bytes) of the one state range (the `state` NVRAM in the machine, a file on
    // the host):
    //   region A: single-asset ledger whose first `single_accounts * 32` bytes are the accounts
    //             drive proved on-chain (cma_ledger_init_single_file, footer at its end)
    //   region B: multi-asset ledger for every other asset, not withdrawable on-chain (libcma
    //             file-mode layout, see ledger.h for how the machine produces it)
    //   region C: meta page (progress, sequencer address, token) + nonce table
    size_t single_accounts{1u << 17};
    size_t region_a_length{32u << 20};
    size_t region_b_length{28u << 20};
    size_t region_c_length{4u << 20};
    size_t multi_accounts{8u * 1024};   // 8192/64/65536 needs ~16 MiB of the 28 MiB region B
    size_t multi_assets{64};
    size_t multi_balances{64u * 1024};

    [[nodiscard]] static Config devnet_defaults();
    // Overrides from QA_* environment variables (docs/build.md); returns false with `err` set when
    // a variable is present but malformed.
    [[nodiscard]] bool apply_env(std::string &err);
    [[nodiscard]] std::string describe_json() const;
    [[nodiscard]] size_t accounts_drive_length() const { return single_accounts * 32; }
    [[nodiscard]] size_t region_c_offset() const { return region_a_length + region_b_length; }
    [[nodiscard]] size_t state_length() const { return region_a_length + region_b_length + region_c_length; }
};

} // namespace qawallet
