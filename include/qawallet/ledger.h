// Ledger facade over libcma. The state is one memory range (the `state` NVRAM in the machine, a
// plain file on the host) holding:
//   region A [0, region_a_length): single-asset ledger for the configured ERC-20
//            (cma_ledger_init_single_file: create-or-open, libcma footer at its end). Its first
//            single_accounts*32 bytes are the accounts drive (uint96 LE balance + address records)
//            that rollups-contracts v3 proves for emergency withdrawals.
//   region B [region_a_length, +region_b_length): multi-asset ledger for every other asset (Ether,
//            other ERC-20s, ERC-721, ERC-1155): credited and withdrawable through vouchers, never
//            on the accounts drive. Its bytes are libcma's *file-mode* layout (heap + footer), so a
//            dump reopens on the host with cma_ledger_init_file at this offset. A UIO range cannot
//            be mapped at a byte offset, so the machine builds the same bytes with
//            cma_ledger_init_buffer over the heap part and writes libcma's footer itself
//            (ledger.cpp, `region_b_footer`); the host unit tests pin that equivalence.
//   region C [region_a_length + region_b_length, +region_c_length): meta page + nonce table
//            (state.h), owned by the engine and exposed here as a raw mapping.
#pragma once

#include "config.h"
#include "types.h"

#include <memory>
#include <string>

namespace qawallet {

enum class AssetKind : uint8_t { Base, Token, TokenId, TokenIdAmount };

struct AssetRef {
    AssetKind kind{AssetKind::Base};
    Address token;
    U256 id;
    [[nodiscard]] static AssetRef ether();
    [[nodiscard]] static AssetRef erc20(const Address &token);
    [[nodiscard]] static AssetRef erc721(const Address &token, const U256 &id);
    [[nodiscard]] static AssetRef erc1155(const Address &token, const U256 &id);
};

// 32-byte account identifier: a wallet address is left-padded with 12 zero bytes.
struct AccountRef {
    bool is_wallet{true};
    Address wallet;
    U256 id;
    [[nodiscard]] static AccountRef of_wallet(const Address &a);
    [[nodiscard]] static AccountRef of_id(const U256 &id);
};

class Ledger {
public:
    // create-or-open; throws std::runtime_error. A regular file is grown to the configured length.
    // `emulate_nvram` makes a regular file take the machine's path for region B (buffer mode + the
    // footer written here): for the tests that pin it against libcma's file mode.
    explicit Ledger(const Config &cfg, const std::string &state_path, bool emulate_nvram = false);
    ~Ledger();
    Ledger(const Ledger &) = delete;
    Ledger &operator=(const Ledger &) = delete;

    [[nodiscard]] bool is_accounts_drive_asset(const AssetRef &a) const; // configured token
    // All return 0 or a negative libcma error code; `last_error()` has the message.
    int deposit(const AccountRef &to, const AssetRef &asset, const U256 &amount);
    int withdraw(const AccountRef &from, const AssetRef &asset, const U256 &amount);
    int transfer(const AccountRef &from, const AccountRef &to, const AssetRef &asset, const U256 &amount);
    int balance(const AccountRef &account, const AssetRef &asset, U256 &out);
    int total_supply(const AssetRef &asset, U256 &out);
    [[nodiscard]] std::string last_error() const;

    [[nodiscard]] bytes_view accounts_drive() const; // the proven prefix of region A
    [[nodiscard]] uint8_t *region_c();               // meta page + nonce table (region_c_length bytes)
    [[nodiscard]] bool is_nvram() const;             // state path is a character device (UIO NVRAM)
    void flush();                                    // msync of the mappings (no-op on an NVRAM)

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

} // namespace qawallet
