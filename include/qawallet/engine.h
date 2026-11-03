// The input-processing engine. Shaped after the sequencer's application-engine.h: a direct input
// or a validated user op goes in, outputs stream out through a sink, the state lives in one memory
// range (ledger.h, state.h), progress is a (count, clock) pair kept in the meta page. The rollup
// device is never touched here; see src/main.cpp (classic driver), src/seq_main.cpp (sequencer
// guest driver) and src/lib/app_engine.cpp (the C ABI the sequencer links).
//
// What an advance input does (docs/inputs.md):
//   sender is a portal            -> libcma deposit, credited to the sender's wallet account
//   one of the 10 libcma selectors -> libcma withdrawal (voucher) or transfer; malformed or
//                                    overdrawn -> reject with a JSON error report
//   anything else                  -> echoed back as one notice with the exact input bytes
#pragma once

#include "config.h"
#include "ledger.h"
#include "sink.h"
#include "types.h"

#include <memory>
#include <stdexcept>
#include <string>

namespace qawallet {

enum class OpenErrorKind : uint8_t { NotFound, InvalidDump, Io };

struct OpenError : std::runtime_error {
    OpenErrorKind kind;
    OpenError(OpenErrorKind k, const std::string &what) : std::runtime_error(what), kind(k) {}
};

// Why a user op was refused (application-engine.h's ApplicationEngineInvalid, minus the reasons
// that belong to the caller).
struct UserOpInvalid {
    enum class Reason : uint8_t { Nonce, FeeBalance } reason{Reason::Nonce};
    uint32_t expected{};  // Nonce
    uint32_t got{};       // Nonce
    U256 required;        // FeeBalance (all ones: no balance could ever cover it)
    U256 available;       // FeeBalance
};

class Engine {
public:
    // Writes a genesis state into a blank range: libcma ledgers created, meta page with count 0,
    // clock 0, the batch submitter address and `cfg.token` (the accounts-drive asset, pinned from
    // then on: opening the state uses the pinned token, not the environment). A regular file is
    // created or grown to the configured length. Throws std::runtime_error, also when the magic
    // is already set.
    static void genesis(const Config &cfg, const std::string &state_path, const Address &sequencer);
    // Whether the range carries a genesis (meta page magic + layout version). Throws OpenError for a
    // missing or unreadable path.
    [[nodiscard]] static bool is_initialized(const Config &cfg, const std::string &state_path);

    // Opens an initialised state; never creates. Throws OpenError (InvalidDump without the magic).
    Engine(Config cfg, const std::string &state_path);
    ~Engine();
    Engine(const Engine &) = delete;
    Engine &operator=(const Engine &) = delete;

    // Never throw. Emit through `sink`. Neither touches the progress: the driver decides what
    // counts (commit_progress / note_reject).
    Completion execute_direct_input(const DirectInput &input, OutputSink &sink);
    Completion inspect(bytes_view payload, OutputSink &sink);

    // User ops (sequencer). validate is pure: nonce must equal the sender's expected nonce and the
    // sender's accounts-drive balance must cover fee::to_linear(current_fee). execute charges
    // fee::to_linear(fee) to the batch submitter's account, advances the nonce by one and runs the
    // payload like a direct input (`in.block_number` is the frame's safe block). It throws
    // std::runtime_error when a validated op cannot pay (an invariant failure, fatal for the engine).
    [[nodiscard]] bool validate_user_op(const Address &sender, uint32_t nonce, uint16_t current_fee,
        UserOpInvalid &invalid) const;
    Completion execute_user_op(const DirectInput &in, uint16_t fee, OutputSink &sink);

    void commit_progress(uint64_t block); // count + 1 (checked), clock = max(clock, block)
    void note_reject();                   // rejected_count + 1

    [[nodiscard]] Progress progress() const;
    [[nodiscard]] uint64_t rejected_count() const;
    [[nodiscard]] uint32_t nonce_of(const Address &sender) const;
    [[nodiscard]] Address sequencer() const;
    [[nodiscard]] Address token() const; // the pinned accounts-drive asset (= config().token)
    // Scheduler facts shown by the `state` inspect report (set by the sequencer guest driver).
    void set_scheduler_status(uint64_t next_batch_nonce, uint64_t queued_directs);
    [[nodiscard]] const Config &config() const;
    [[nodiscard]] Ledger &ledger();
    void flush();

private:
    struct Impl;
    std::unique_ptr<Impl> m;
};

// Whether `payload` starts with one of libcma's 10 advance selectors (WithdrawEther ...
// TransferErc1155Batch, libcma/parser.h); such a payload is a ledger input, never an echo.
[[nodiscard]] bool is_libcma_selector(bytes_view payload);

} // namespace qawallet
