// qa-wallet: basic value types shared by the engine, the ledger and the rollup drivers.
// Everything here is plain data; no libcmt/libcma types leak through the engine API.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace qawallet {

using bytes = std::vector<uint8_t>;
using bytes_view = std::span<const uint8_t>;

struct Address {
    std::array<uint8_t, 20> b{};
    bool operator==(const Address &) const = default;
    [[nodiscard]] bool is_zero() const;
};

// 256-bit unsigned integer, big-endian (EVM word order).
struct U256 {
    std::array<uint8_t, 32> b{};
    bool operator==(const U256 &) const = default;
    [[nodiscard]] static U256 from_u64(uint64_t v);
    [[nodiscard]] bool fits_u64() const;
    [[nodiscard]] uint64_t to_u64() const; // low 64 bits
    [[nodiscard]] bool is_zero() const;
};

// Mirrors ApplicationEngineDirectInput from the sequencer's application-engine.h, plus the
// metadata a Cartesi guest also receives. The sequencer only knows sender/block/payload.
struct DirectInput {
    Address sender;
    uint64_t block_number{};
    bytes_view payload;
    // guest-only metadata
    uint64_t block_timestamp{};
    uint64_t index{};
    uint64_t chain_id{};
    Address app_contract;
};

// What the rollup driver must do after the engine returns.
enum class Completion : uint8_t {
    Accept, // manual yield RX_ACCEPTED with the outputs root
    Reject, // manual yield RX_REJECTED (state reverted by the host)
};

struct Progress {
    uint64_t executed_input_count{}; // accepted advances since the state was created
    uint64_t last_block{};           // max block number seen in an accepted advance
};

[[nodiscard]] std::string to_hex(bytes_view data);              // "0x..."
[[nodiscard]] std::string to_hex(const Address &a);
[[nodiscard]] std::string to_hex(const U256 &v);
[[nodiscard]] bool from_hex(std::string_view text, bytes &out); // accepts optional 0x
[[nodiscard]] bool parse_address(std::string_view text, Address &out);
[[nodiscard]] bool parse_u256(std::string_view text, U256 &out); // 0x hex (<= 32 bytes) or decimal
[[nodiscard]] std::string u256_to_dec(const U256 &v);

} // namespace qawallet
