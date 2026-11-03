// qa-wallet-seq: the sequencer guest driver. Owns the rollup device (libcmt) like src/main.cpp,
// but hands every advance input to the canonical scheduler fold of the Cartesi sequencer
// (seq/guest, include/qaseq.h), which reaches the engine through the application-engine C ABI,
// exactly as the sequencer does on the host. Inputs from the batch submitter are batches, every
// other sender is a direct input queued until a frame covers it or it becomes overdue; a rejected
// ledger input is a counted no-op, so the only yields are RX_ACCEPTED.
//   qa-wallet-seq [state range] [config range]   (defaults /dev/uio0 /dev/uio1)
// The config range holds the batch submitter address in bytes 0..20 and the accounts-drive ERC-20
// in bytes 20..40 (zero: the QA_TOKEN / devnet default), the rest zero (docs/sequencer.md). A blank
// state gets its genesis on the first boot, pinning both.
#include "cmt_sink.h"
#include "qaseq.h"
#include "qawallet/config.h"
#include "qawallet/engine.h"
#include "qawallet/seq_ext.h"
#include "qawallet/types.h"

extern "C" {
#include <libcmt/abi.h>
#include <libcmt/io.h>
#include <libcmt/rollup.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
}

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>

using namespace qawallet;

namespace {

bool g_log = true;
constexpr size_t CONFIG_RANGE_LENGTH = 4096;

struct RangeConfig {
    Address sequencer;
    Address token; // zero when the range leaves the default
};

__attribute__((format(printf, 1, 2))) void logf(const char *fmt, ...) {
    if (!g_log) {
        return;
    }
    std::va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[qa-wallet-seq] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

[[noreturn]] void die(const char *what, const char *detail) {
    std::fprintf(stderr, "[qa-wallet-seq] %s: %s\n", what, detail);
    sync();
    std::exit(1); // the init halts the machine: a fatal fold error is a fixed point, by design
}

// Reads the config range (mapped: a UIO range cannot be read()).
RangeConfig read_config_range(const char *path) {
    int fd = ::open(path, O_RDWR);
    if (fd < 0) {
        die("cannot open the config range", std::strerror(errno));
    }
    void *p = ::mmap(nullptr, CONFIG_RANGE_LENGTH, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        die("cannot map the config range", std::strerror(errno));
    }
    const auto *bytes = static_cast<const uint8_t *>(p);
    RangeConfig c;
    std::copy_n(bytes, 20, c.sequencer.b.begin());
    std::copy_n(bytes + 20, 20, c.token.b.begin());
    bool tail_zero = std::all_of(bytes + 40, bytes + CONFIG_RANGE_LENGTH, [](uint8_t x) { return x == 0; });
    ::munmap(p, CONFIG_RANGE_LENGTH);
    if (c.sequencer.is_zero() || !tail_zero) {
        die("config range", "expected the batch submitter address, the token address (or zeros) and zeros (docs/sequencer.md)");
    }
    return c;
}

// Reports reach the device through the aux sink (notices and vouchers cross the seam).
int aux_report(void *ctx, const uint8_t *data, uint64_t size) {
    return static_cast<CmtSink *>(ctx)->report(bytes_view(data, size));
}

int emit_outputs(qaseq_t *fold, CmtSink &sink) {
    uint64_t n = qaseq_output_count(fold);
    for (uint64_t i = 0; i < n; ++i) {
        qaseq_output_t o{};
        if (qaseq_output(fold, i, &o) != 0) {
            return -EINVAL;
        }
        bytes_view payload(o.data, o.size);
        int rc = 0;
        if (o.kind == QASEQ_OUTPUT_VOUCHER) {
            Address dest;
            U256 value;
            std::copy(std::begin(o.destination), std::end(o.destination), dest.b.begin());
            std::copy(std::begin(o.value), std::end(o.value), value.b.begin());
            rc = sink.voucher(dest, value, payload);
        } else {
            rc = sink.notice(payload);
        }
        if (rc != 0) {
            return rc;
        }
    }
    return 0;
}

} // namespace

int main(int argc, char *argv[]) {
    if (std::setvbuf(stderr, nullptr, _IONBF, 0) != 0) {
        return 1;
    }
    Config cfg = Config::devnet_defaults();
    std::string err;
    if (!cfg.apply_env(err)) {
        std::fprintf(stderr, "[qa-wallet-seq] bad configuration: %s\n", err.c_str());
        return 1;
    }
    g_log = cfg.log;
    const char *state = argc > 1 ? argv[1] : "/dev/uio0";
    const char *config_range = argc > 2 ? argv[2] : "/dev/uio1";
    logf("state range %s, config range %s", state, config_range);

    RangeConfig rc_cfg = read_config_range(config_range);
    const Address sequencer = rc_cfg.sequencer;
    if (!rc_cfg.token.is_zero()) {
        cfg.token = rc_cfg.token;
    }
    logf("config %s", cfg.describe_json().c_str());
    logf("batch submitter %s, accounts-drive token %s", to_hex(sequencer).c_str(), to_hex(cfg.token).c_str());
    try {
        if (!Engine::is_initialized(cfg, state)) {
            logf("blank state: writing the genesis");
            if (qawallet_genesis(state, sequencer.b.data(), cfg.token.b.data()) != 0) {
                die("genesis failed", application_engine_get_last_error_message());
            }
        }
    } catch (const std::exception &e) {
        die("cannot inspect the state", e.what());
    }

    qaseq_t *fold = qaseq_new(state, sequencer.b.data(), 0);
    if (fold == nullptr) {
        die("cannot start the scheduler fold", qaseq_last_error());
    }
    {
        // the engine's pinned submitter and token must be the range's (a replaced range on a used state)
        std::string got;
        auto cb = [](void *c, const uint8_t *d, uint64_t n) {
            static_cast<std::string *>(c)->assign(reinterpret_cast<const char *>(d), n);
            return 0;
        };
        const char *q = "state";
        if (qawallet_live_inspect(reinterpret_cast<const uint8_t *>(q), 5, cb, &got) != 0 ||
            got.find("\"sequencer_address\":\"" + to_hex(sequencer) + "\"") == std::string::npos ||
            got.find("\"token\":\"" + to_hex(cfg.token) + "\"") == std::string::npos) {
            die("config range", ("the state was created for another batch submitter or token: " + got).c_str());
        }
    }

    cmt_rollup_t rollup{};
    int rc = cmt_rollup_init(&rollup);
    if (rc != 0) {
        die("unable to initialise the rollup device", std::strerror(-rc));
    }
    CmtSink sink(&rollup);
    qawallet_aux_sink aux{&sink, aux_report};
    if (qawallet_live_attach_aux(&aux) != 0) {
        die("cannot attach the device sink", "no live engine");
    }
    sync();

    logf("processing rollup requests (max wait %llu blocks)...", static_cast<unsigned long long>(qaseq_max_wait_blocks()));
    while (true) {
        cmt_rollup_finish_t fin{};
        fin.accept_previous_request = true;
        rc = cmt_rollup_finish(&rollup, &fin);
        if (rc != 0) {
            die("rollup finish failed", std::strerror(-rc));
        }
        if (fin.next_request_type == HTIF_YIELD_REASON_ADVANCE_STATE) {
            cmt_rollup_advance_t adv{};
            rc = cmt_rollup_read_advance_state(&rollup, &adv);
            if (rc != 0) {
                die("unable to read advance state", std::strerror(-rc));
            }
            qawallet_input_context ctx{};
            ctx.block_timestamp = adv.block_timestamp;
            ctx.index = adv.index;
            ctx.chain_id = adv.chain_id;
            std::memcpy(ctx.app_contract, adv.app_contract.data, 20);
            (void) qawallet_live_set_input_context(&ctx);
            Address sender;
            std::memcpy(sender.b.data(), adv.msg_sender.data, 20);
            logf("advance #%llu from %s (%zu bytes) at block %llu", static_cast<unsigned long long>(adv.index),
                to_hex(sender).c_str(), static_cast<size_t>(adv.payload.length),
                static_cast<unsigned long long>(adv.block_number));
            qaseq_outcome_t outcome{};
            if (qaseq_process_input(fold, adv.msg_sender.data, adv.block_number, adv.chain_id, adv.app_contract.data,
                    static_cast<const uint8_t *>(adv.payload.data), adv.payload.length, &outcome) != 0) {
                die("scheduler fold failed", qaseq_last_error());
            }
            rc = emit_outputs(fold, sink);
            if (rc != 0) {
                die("unable to emit an output", std::strerror(-rc));
            }
            if (outcome.kind == QASEQ_BATCH_REJECTED) {
                static const char DROPPED[] = "scheduler dropped invalid batch"; // canonical-app's report
                (void) sink.report(bytes_view(reinterpret_cast<const uint8_t *>(DROPPED), sizeof DROPPED - 1));
            }
            (void) qawallet_live_set_scheduler_status(qaseq_next_batch_nonce(fold), qaseq_queued_directs(fold));
            sync();
            logf("advance #%llu outcome %u (reason %u), %llu outputs, next batch nonce %llu, %llu queued directs",
                static_cast<unsigned long long>(adv.index), outcome.kind, outcome.reject_reason,
                static_cast<unsigned long long>(qaseq_output_count(fold)),
                static_cast<unsigned long long>(qaseq_next_batch_nonce(fold)),
                static_cast<unsigned long long>(qaseq_queued_directs(fold)));
        } else if (fin.next_request_type == HTIF_YIELD_REASON_INSPECT_STATE) {
            cmt_rollup_inspect_t ins{};
            rc = cmt_rollup_read_inspect_state(&rollup, &ins);
            if (rc != 0) {
                die("unable to read inspect state", std::strerror(-rc));
            }
            logf("inspect (%zu bytes)", static_cast<size_t>(ins.payload.length));
            (void) qawallet_live_set_scheduler_status(qaseq_next_batch_nonce(fold), qaseq_queued_directs(fold));
            rc = qawallet_live_inspect(static_cast<const uint8_t *>(ins.payload.data), ins.payload.length, aux_report, &sink);
            if (rc != 0) {
                logf("inspect failed: %s", application_engine_get_last_error_message());
            }
        } else {
            die("unknown request type", std::to_string(fin.next_request_type).c_str());
        }
    }
}
