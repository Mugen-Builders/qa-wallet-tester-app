// qa-wallet: rollup driver. Owns the rollup device (libcmt) and nothing else; every decision is
// made by qawallet::Engine (docs/inputs.md: libcma deposits, withdrawals and transfers act on the
// ledger, every other input is echoed back as a notice).
#include "cmt_sink.h"
#include "qawallet/config.h"
#include "qawallet/engine.h"
#include "qawallet/sink.h"
#include "qawallet/types.h"

extern "C" {
#include <libcmt/abi.h>
#include <libcmt/io.h>
#include <libcmt/rollup.h>
#include <sys/stat.h>
#include <unistd.h>
}

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

using namespace qawallet;

namespace {

const char *g_merkle_path = "/tmp/merkle";
bool g_log = true;

__attribute__((format(printf, 1, 2))) void logf(const char *fmt, ...) {
    if (!g_log) {
        return;
    }
    std::va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[qa-wallet] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

void save_merkle(cmt_rollup_t *r) {
    mode_t old = umask(0000);
    int rc = cmt_rollup_save_merkle(r, g_merkle_path);
    umask(old);
    if (rc != 0) {
        logf("warning: unable to save merkle tree: %s", std::strerror(-rc));
    }
}

// Manual RX_REJECTED yield. On the real machine it never returns: the host restores the snapshot
// taken at the previous RX_ACCEPTED yield and the guest resumes from there. The libcmt mock does
// return, with the next request in the reply, which is what the mock e2e test uses; the reply's
// payload length is recorded where cmt_rollup_finish records it, so the next read sees it.
// Returns the next request type (>= 0) or a negative errno.
int reject_yield(cmt_rollup_t *r, uint32_t *next_length) {
    cmt_io_yield_t req{HTIF_DEVICE_YIELD, HTIF_YIELD_CMD_MANUAL, HTIF_YIELD_MANUAL_REASON_RX_REJECTED, 0};
    int rc = cmt_io_yield(r->io, &req);
    if (rc != 0) {
        return rc;
    }
    r->fromhost_data = req.data;
    *next_length = req.data;
    return req.reason;
}

} // namespace

int main(int argc, char *argv[]) {
    if (std::setvbuf(stderr, nullptr, _IONBF, 0) != 0) {
        return 1;
    }
    Config cfg = Config::devnet_defaults();
    std::string err;
    if (!cfg.apply_env(err)) {
        std::fprintf(stderr, "[qa-wallet] bad configuration: %s\n", err.c_str());
        return 1;
    }
    g_log = cfg.log;
    // argv: [state range]. In the machine it is the `state` NVRAM (/dev/uio0, see cartesi.toml);
    // on the host a plain file. A blank state gets its genesis on the first boot.
    const char *state = argc > 1 ? argv[1] : std::getenv("QA_STATE_DEVICE");
    if (state == nullptr || *state == '\0') {
        state = "/dev/uio0";
    }
    if (const char *mp = std::getenv("QA_MERKLE_PATH"); mp != nullptr && *mp != '\0') {
        g_merkle_path = mp;
    }
    logf("config %s", cfg.describe_json().c_str());
    logf("state range %s", state);

    Engine *engine = nullptr;
    try {
        if (!Engine::is_initialized(cfg, state)) {
            logf("blank state: writing the genesis");
            Engine::genesis(cfg, state, Address{});
        }
        engine = new Engine(cfg, state);
    } catch (const std::exception &e) {
        std::fprintf(stderr, "[qa-wallet] unable to initialise the ledger: %s\n", e.what());
        return 1;
    }

    cmt_rollup_t rollup{};
    int rc = cmt_rollup_init(&rollup);
    if (rc != 0) {
        std::fprintf(stderr, "[qa-wallet] unable to initialise the rollup device: %s\n", std::strerror(-rc));
        return 1;
    }
    struct stat st{};
    if (stat(g_merkle_path, &st) == 0) {
        rc = cmt_rollup_load_merkle(&rollup, g_merkle_path);
        if (rc != 0) {
            std::fprintf(stderr, "[qa-wallet] unable to load merkle tree: %s\n", std::strerror(-rc));
            return 1;
        }
    }
    sync(); // a flash-drive state goes through the page cache; an NVRAM does not need this

    CmtSink sink(&rollup);
    Completion pending = Completion::Accept;
    logf("processing rollup requests...");
    while (true) {
        int next_type = 0;
        uint32_t next_len = 0;
        if (pending == Completion::Accept) {
            save_merkle(&rollup);
            cmt_rollup_finish_t fin{};
            fin.accept_previous_request = true;
            rc = cmt_rollup_finish(&rollup, &fin);
            if (rc != 0) {
                std::fprintf(stderr, "[qa-wallet] rollup finish failed: %s\n", std::strerror(-rc));
                return 1;
            }
            next_type = fin.next_request_type;
            next_len = fin.next_request_payload_length;
        } else {
            next_type = reject_yield(&rollup, &next_len);
        }
        if (next_type < 0) {
            std::fprintf(stderr, "[qa-wallet] yield failed: %s\n", std::strerror(-next_type));
            return 1;
        }
        (void) next_len;

        if (next_type == HTIF_YIELD_REASON_ADVANCE_STATE) {
            cmt_rollup_advance_t adv{};
            rc = cmt_rollup_read_advance_state(&rollup, &adv);
            if (rc != 0) {
                std::fprintf(stderr, "[qa-wallet] unable to read advance state: %s\n", std::strerror(-rc));
                return 1;
            }
            DirectInput in;
            std::memcpy(in.sender.b.data(), adv.msg_sender.data, 20);
            std::memcpy(in.app_contract.b.data(), adv.app_contract.data, 20);
            in.block_number = adv.block_number;
            in.block_timestamp = adv.block_timestamp;
            in.index = adv.index;
            in.chain_id = adv.chain_id;
            in.payload = bytes_view(static_cast<const uint8_t *>(adv.payload.data), adv.payload.length);
            logf("advance #%llu from %s (%zu bytes)", static_cast<unsigned long long>(adv.index),
                to_hex(in.sender).c_str(), in.payload.size());
            pending = engine->execute_direct_input(in, sink);
            if (pending == Completion::Accept) {
                engine->commit_progress(in.block_number);
            } else {
                engine->note_reject();
            }
            engine->flush();
            sync();
            logf("advance #%llu completion %d", static_cast<unsigned long long>(adv.index), static_cast<int>(pending));
        } else if (next_type == HTIF_YIELD_REASON_INSPECT_STATE) {
            cmt_rollup_inspect_t ins{};
            rc = cmt_rollup_read_inspect_state(&rollup, &ins);
            if (rc != 0) {
                std::fprintf(stderr, "[qa-wallet] unable to read inspect state: %s\n", std::strerror(-rc));
                return 1;
            }
            bytes_view q(static_cast<const uint8_t *>(ins.payload.data), ins.payload.length);
            logf("inspect (%zu bytes)", q.size());
            pending = engine->inspect(q, sink);
            logf("inspect completion %d", static_cast<int>(pending));
        } else {
            std::fprintf(stderr, "[qa-wallet] unknown request type %d\n", next_type);
            return 1;
        }
    }
}
