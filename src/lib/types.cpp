#include "qawallet/types.h"

#include <algorithm>
#include <cstdio>

namespace qawallet {

bool Address::is_zero() const {
    return std::all_of(b.begin(), b.end(), [](uint8_t x) { return x == 0; });
}

U256 U256::from_u64(uint64_t v) {
    U256 r;
    for (int i = 0; i < 8; ++i) {
        r.b[31 - i] = static_cast<uint8_t>(v & 0xffU);
        v >>= 8;
    }
    return r;
}

bool U256::fits_u64() const {
    return std::all_of(b.begin(), b.begin() + 24, [](uint8_t x) { return x == 0; });
}

uint64_t U256::to_u64() const {
    uint64_t v = 0;
    for (int i = 24; i < 32; ++i) {
        v = (v << 8) | b[i];
    }
    return v;
}

bool U256::is_zero() const {
    return std::all_of(b.begin(), b.end(), [](uint8_t x) { return x == 0; });
}

static const char HEX[] = "0123456789abcdef";

std::string to_hex(bytes_view data) {
    std::string s;
    s.reserve(2 + 2 * data.size());
    s += "0x";
    for (uint8_t x : data) {
        s += HEX[x >> 4];
        s += HEX[x & 0xf];
    }
    return s;
}

std::string to_hex(const Address &a) {
    return to_hex(bytes_view(a.b.data(), a.b.size()));
}

std::string to_hex(const U256 &v) {
    return to_hex(bytes_view(v.b.data(), v.b.size()));
}

static int nibble(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

bool from_hex(std::string_view text, bytes &out) {
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        text.remove_prefix(2);
    }
    if (text.size() % 2 != 0) {
        return false;
    }
    out.clear();
    out.reserve(text.size() / 2);
    for (size_t i = 0; i < text.size(); i += 2) {
        int hi = nibble(text[i]);
        int lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) {
            return false;
        }
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return true;
}

bool parse_address(std::string_view text, Address &out) {
    bytes tmp;
    if (!from_hex(text, tmp) || tmp.size() != 20) {
        return false;
    }
    std::copy(tmp.begin(), tmp.end(), out.b.begin());
    return true;
}

bool parse_u256(std::string_view text, U256 &out) {
    out = U256{};
    if (text.size() >= 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        std::string_view hex = text.substr(2);
        if (hex.empty() || hex.size() > 64) {
            return false;
        }
        std::string padded(64 - hex.size(), '0');
        padded.append(hex);
        bytes tmp;
        if (!from_hex(padded, tmp)) {
            return false;
        }
        std::copy(tmp.begin(), tmp.end(), out.b.begin());
        return true;
    }
    if (text.empty()) {
        return false;
    }
    // decimal: multiply-accumulate on the big-endian array
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        unsigned carry = static_cast<unsigned>(c - '0');
        for (int i = 31; i >= 0; --i) {
            unsigned v = out.b[i] * 10U + carry;
            out.b[i] = static_cast<uint8_t>(v & 0xffU);
            carry = v >> 8;
        }
        if (carry != 0) {
            return false; // overflow
        }
    }
    return true;
}

std::string u256_to_dec(const U256 &v) {
    std::array<uint8_t, 32> n = v.b;
    std::string digits;
    bool nonzero = true;
    while (nonzero) {
        unsigned rem = 0;
        nonzero = false;
        for (auto &x : n) {
            unsigned cur = (rem << 8) | x;
            x = static_cast<uint8_t>(cur / 10);
            rem = cur % 10;
            if (x != 0) {
                nonzero = true;
            }
        }
        digits.push_back(static_cast<char>('0' + rem));
    }
    std::reverse(digits.begin(), digits.end());
    return digits;
}

} // namespace qawallet
