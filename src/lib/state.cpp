#include "qawallet/state.h"

#include <algorithm>
#include <cstring>

namespace qawallet {

namespace {

constexpr char MAGIC[8] = {'Q', 'A', 'T', 'E', 'S', 'T', 'R', '1'};
constexpr size_t OFF_MAGIC = 0;
constexpr size_t OFF_VERSION = 8;
constexpr size_t OFF_COUNT = 16;
constexpr size_t OFF_CLOCK = 24;
constexpr size_t OFF_REJECTED = 32;
constexpr size_t OFF_SEQUENCER = 48;
constexpr size_t OFF_TOKEN = 68;
constexpr size_t OFF_NONCE_COUNT = 88;

uint64_t rd64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) {
        v = (v << 8) | p[i];
    }
    return v;
}

void wr64(uint8_t *p, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        p[i] = static_cast<uint8_t>(v & 0xffU);
        v >>= 8;
    }
}

uint32_t rd32(const uint8_t *p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
        (static_cast<uint32_t>(p[3]) << 24);
}

void wr32(uint8_t *p, uint32_t v) {
    for (int i = 0; i < 4; ++i) {
        p[i] = static_cast<uint8_t>(v & 0xffU);
        v >>= 8;
    }
}

} // namespace

bool MetaView::has_magic() const {
    return std::memcmp(page + OFF_MAGIC, MAGIC, sizeof MAGIC) == 0 && rd32(page + OFF_VERSION) == STATE_LAYOUT_VERSION;
}

void MetaView::initialize(const Address &sequencer, const Address &token) {
    std::memset(page, 0, META_PAGE_LENGTH);
    std::memcpy(page + OFF_MAGIC, MAGIC, sizeof MAGIC);
    wr32(page + OFF_VERSION, STATE_LAYOUT_VERSION);
    std::copy(sequencer.b.begin(), sequencer.b.end(), page + OFF_SEQUENCER);
    std::copy(token.b.begin(), token.b.end(), page + OFF_TOKEN);
}

uint64_t MetaView::executed_input_count() const {
    return rd64(page + OFF_COUNT);
}
uint64_t MetaView::last_executed_safe_block() const {
    return rd64(page + OFF_CLOCK);
}
uint64_t MetaView::rejected_count() const {
    return rd64(page + OFF_REJECTED);
}
Address MetaView::sequencer() const {
    Address a;
    std::copy_n(page + OFF_SEQUENCER, 20, a.b.begin());
    return a;
}
Address MetaView::token() const {
    Address a;
    std::copy_n(page + OFF_TOKEN, 20, a.b.begin());
    return a;
}
uint64_t MetaView::nonce_count() const {
    return rd64(page + OFF_NONCE_COUNT);
}
void MetaView::set_progress(uint64_t count, uint64_t clock) {
    wr64(page + OFF_COUNT, count);
    wr64(page + OFF_CLOCK, clock);
}
void MetaView::set_rejected_count(uint64_t v) {
    wr64(page + OFF_REJECTED, v);
}
void MetaView::set_nonce_count(uint64_t v) {
    wr64(page + OFF_NONCE_COUNT, v);
}

NonceTable::NonceTable(uint8_t *base, size_t length, MetaView meta)
    : m_base(base), m_slots(length / NONCE_SLOT_LENGTH), m_meta(meta) {}

size_t NonceTable::home(const Address &a) const {
    return static_cast<size_t>(rd32(a.b.data() + 16)) % m_slots;
}

long NonceTable::find(const Address &a) const {
    if (a.is_zero() || m_slots == 0) {
        return -1;
    }
    size_t i = home(a);
    for (size_t probes = 0; probes < m_slots; ++probes) {
        const uint8_t *slot = m_base + i * NONCE_SLOT_LENGTH;
        if (std::all_of(slot, slot + 20, [](uint8_t x) { return x == 0; })) {
            return -1; // empty slot ends the probe chain
        }
        if (std::equal(a.b.begin(), a.b.end(), slot)) {
            return static_cast<long>(i);
        }
        i = (i + 1) % m_slots;
    }
    return -1;
}

uint32_t NonceTable::get(const Address &a) const {
    long i = find(a);
    if (i < 0) {
        return 0;
    }
    return rd32(m_base + static_cast<size_t>(i) * NONCE_SLOT_LENGTH + 20);
}

bool NonceTable::can_insert(const Address &a) const {
    if (a.is_zero() || m_slots == 0) {
        return false;
    }
    if (find(a) >= 0) {
        return true;
    }
    return m_meta.nonce_count() < m_slots / 2;
}

bool NonceTable::set(const Address &a, uint32_t nonce) {
    if (!can_insert(a)) {
        return false;
    }
    long found = find(a);
    size_t i;
    if (found >= 0) {
        i = static_cast<size_t>(found);
    } else {
        i = home(a);
        while (!std::all_of(m_base + i * NONCE_SLOT_LENGTH, m_base + i * NONCE_SLOT_LENGTH + 20,
            [](uint8_t x) { return x == 0; })) {
            i = (i + 1) % m_slots;
        }
        uint8_t *slot = m_base + i * NONCE_SLOT_LENGTH;
        std::memset(slot, 0, NONCE_SLOT_LENGTH);
        std::copy(a.b.begin(), a.b.end(), slot);
        m_meta.set_nonce_count(m_meta.nonce_count() + 1);
    }
    wr32(m_base + i * NONCE_SLOT_LENGTH + 20, nonce);
    return true;
}

} // namespace qawallet
