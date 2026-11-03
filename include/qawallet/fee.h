// Log-space fee encoding of the Cartesi sequencer (sequencer-core/src/fee.rs, v0.1.0-alpha.10): a
// fee is a uint16 exponent e with linear value (129/128)^e, computed in fixed point with 64
// fractional bits from a table of (129/128)^(2^i), i < 15, built by repeated squaring with
// floor((a*b) >> 64) at every step. Native and canonical builds must agree bit for bit, which is why
// this is a literal port of that arithmetic and the Rust tests compare every exponent.
#pragma once

#include "types.h"

#include <cstdint>

namespace qawallet::fee {

// Largest exponent whose fixed-point value fits 256 bits (sequencer-core's MAX_EXPONENT).
[[nodiscard]] uint16_t max_exponent();
// (129/128)^e in the fee token's smallest unit (the fixed-point value shifted right by 64).
// Returns false for e > max_exponent() (no finite amount: callers treat it as uncoverable).
[[nodiscard]] bool to_linear(uint16_t e, U256 &out);

} // namespace qawallet::fee
