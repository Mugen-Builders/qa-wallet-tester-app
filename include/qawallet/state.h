// Region C of the ledger range: the meta page and the nonce table. Everything an engine restart or a
// host/machine byte comparison must see lives here, in fixed little-endian layouts:
//
//   meta page (4096 bytes at region C offset 0)
//     0   8  magic "QATESTR1"
//     8   4  layout version (1)
//     16  8  executed_input_count     next application-history offset (included no-ops count)
//     24  8  last_executed_safe_block max block carried by an executed input
//     32  8  rejected_count           inputs the engine refused (QA statistic)
//     40  1  reserved (zero; the tester's deposit policy byte)
//     48  20 sequencer address        batch submitter; fee recipient; pinned at genesis
//     68  20 token address            the accounts-drive ERC-20 (region A's asset); pinned at genesis
//     88  8  nonce_count              populated nonce-table slots
//   nonce table (the rest of region C, 32-byte slots)
//     20 address | 4 nonce (u32 LE) | 8 zero. Open addressing: slot = low 32 bits of the address
//     mod the slot count, linear probing; the zero address marks an empty slot. Insertion stops at
//     50 % load so the probe length stays bounded and the layout stays deterministic.
#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>

namespace qawallet {

constexpr size_t META_PAGE_LENGTH = 4096;
constexpr size_t NONCE_SLOT_LENGTH = 32;
constexpr uint32_t STATE_LAYOUT_VERSION = 1;

struct MetaView {
    uint8_t *page{nullptr};

    [[nodiscard]] bool has_magic() const;
    void initialize(const Address &sequencer, const Address &token); // magic, version, zero counters

    [[nodiscard]] uint64_t executed_input_count() const;
    [[nodiscard]] uint64_t last_executed_safe_block() const;
    [[nodiscard]] uint64_t rejected_count() const;
    [[nodiscard]] Address sequencer() const;
    [[nodiscard]] Address token() const;
    [[nodiscard]] uint64_t nonce_count() const;

    void set_progress(uint64_t count, uint64_t clock);
    void set_rejected_count(uint64_t v);
    void set_nonce_count(uint64_t v);
};

class NonceTable {
public:
    NonceTable(uint8_t *base, size_t length, MetaView meta); // `base` = region C + META_PAGE_LENGTH
    [[nodiscard]] uint32_t get(const Address &a) const;       // 0 when absent
    // Sets the nonce of `a`, inserting it when absent. Returns false when the table refuses a new
    // sender (50 % load reached); the table is unchanged then.
    [[nodiscard]] bool set(const Address &a, uint32_t nonce);
    [[nodiscard]] bool can_insert(const Address &a) const;    // true when `a` is present or there is room
    [[nodiscard]] size_t slots() const { return m_slots; }

private:
    [[nodiscard]] long find(const Address &a) const; // slot index or -1
    [[nodiscard]] size_t home(const Address &a) const;
    uint8_t *m_base;
    size_t m_slots;
    MetaView m_meta;
};

} // namespace qawallet
