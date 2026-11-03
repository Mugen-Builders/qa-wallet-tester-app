#include "qawallet/fee.h"

#include <array>

namespace qawallet::fee {

namespace {

// 256-bit unsigned in four little-endian 64-bit limbs (alloy's U256 layout).
struct Limbs {
    std::array<uint64_t, 4> l{};
};

constexpr unsigned FRAC_BITS = 64;

__extension__ typedef unsigned __int128 u128;

// floor((a * b) >> 64); `ok` is false when the result does not fit 256 bits.
Limbs fixed_mul(const Limbs &a, const Limbs &b, bool &ok) {
    std::array<uint64_t, 8> p{};
    for (int i = 0; i < 4; ++i) {
        u128 carry = 0;
        for (int j = 0; j < 4; ++j) {
            u128 cur = static_cast<u128>(a.l[i]) * b.l[j] + p[i + j] + carry;
            p[i + j] = static_cast<uint64_t>(cur);
            carry = cur >> 64;
        }
        p[i + 4] = static_cast<uint64_t>(carry);
    }
    // shift right by one limb (64 bits); limbs 5..7 of the product must be zero to fit
    ok = (p[5] == 0 && p[6] == 0 && p[7] == 0);
    Limbs r;
    r.l = {p[1], p[2], p[3], p[4]};
    return r;
}

struct Table {
    std::array<Limbs, 15> t{};
    uint16_t max_exponent{0};
    Table() {
        // (129/128) in fixed point: 129 * 2^64 / 128 = 129 * 2^57 (needs 65 bits)
        t[0].l = {static_cast<uint64_t>(129) << 57, static_cast<uint64_t>(129) >> 7, 0, 0};
        bool ok = true;
        for (int i = 1; i < 15; ++i) {
            t[i] = fixed_mul(t[i - 1], t[i - 1], ok);
            // sequencer-core/build.rs panics on overflow here; the pinned table fits.
        }
        // greedy from the most significant bit: keep a bit when the product still fits
        Limbs value;
        value.l = {0, 1, 0, 0}; // 1.0
        uint16_t exp = 0;
        for (int i = 14; i >= 0; --i) {
            Limbs cand = fixed_mul(value, t[i], ok);
            if (ok) {
                exp = static_cast<uint16_t>(exp | (1u << i));
                value = cand;
            }
        }
        max_exponent = exp;
    }
};

const Table &table() {
    static const Table t;
    return t;
}

} // namespace

uint16_t max_exponent() {
    return table().max_exponent;
}

bool to_linear(uint16_t e, U256 &out) {
    out = U256{};
    const Table &tb = table();
    if (e > tb.max_exponent) {
        return false;
    }
    Limbs v;
    v.l = {0, 1, 0, 0};
    bool ok = true;
    for (int i = 0; i < 15; ++i) {
        if ((e & (1u << i)) != 0) {
            v = fixed_mul(v, tb.t[i], ok);
            if (!ok) {
                return false; // unreachable for e <= max_exponent
            }
        }
    }
    // linear = fixed >> 64: drop limb 0; big-endian bytes out
    std::array<uint64_t, 4> lin = {v.l[1], v.l[2], v.l[3], 0};
    for (int limb = 0; limb < 4; ++limb) {
        uint64_t x = lin[limb];
        for (int b = 0; b < 8; ++b) {
            out.b[31 - (limb * 8 + b)] = static_cast<uint8_t>(x & 0xffU);
            x >>= 8;
        }
    }
    (void) FRAC_BITS;
    return true;
}

} // namespace qawallet::fee
