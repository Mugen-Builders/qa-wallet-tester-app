// CmtSink: streams engine emissions straight into the rollup device (libcmt). Shared by the
// classic driver (src/main.cpp) and the sequencer guest driver (src/seq_main.cpp).
#pragma once

#include "qawallet/sink.h"
#include "qawallet/types.h"

extern "C" {
#include <libcmt/abi.h>
#include <libcmt/rollup.h>
}

#include <cstring>

namespace qawallet {

class CmtSink final : public OutputSink {
public:
    explicit CmtSink(cmt_rollup_t *r) : m_r(r) {}

    int notice(bytes_view payload) override {
        cmt_abi_bytes_t b{payload.size(), const_cast<uint8_t *>(payload.data())};
        return cmt_rollup_emit_notice(m_r, &b, nullptr);
    }
    int voucher(const Address &destination, const U256 &value, bytes_view payload) override {
        cmt_abi_address_t a{};
        cmt_abi_u256_t v{};
        std::memcpy(a.data, destination.b.data(), 20);
        std::memcpy(v.data, value.b.data(), 32);
        cmt_abi_bytes_t b{payload.size(), const_cast<uint8_t *>(payload.data())};
        return cmt_rollup_emit_voucher(m_r, &a, &v, &b, nullptr);
    }
    int report(bytes_view payload) override {
        cmt_abi_bytes_t b{payload.size(), const_cast<uint8_t *>(payload.data())};
        return cmt_rollup_emit_report(m_r, &b);
    }

private:
    cmt_rollup_t *m_r;
};

} // namespace qawallet
