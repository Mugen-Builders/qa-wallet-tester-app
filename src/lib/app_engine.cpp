// The sequencer's application-engine.h seam (v0.1.0-alpha.10) over qawallet::Engine, plus the
// qa-wallet extensions of seq_ext.h. Rules of the seam honoured here: every entry point is total
// and noexcept, statuses are the contract, messages are thread-local diagnostics, a dump opened
// with from_dump is never written to (the host works on a private copy; a live machine range, a
// character device, is worked on in place), rejected inputs are counted no-ops.
#include "qawallet/application-engine.h"
#include "qawallet/seq_ext.h"

#include "qawallet/config.h"
#include "qawallet/engine.h"
#include "qawallet/fee.h"
#include "qawallet/sink.h"
#include "qawallet/types.h"

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

using namespace qawallet;

// Layouts the seam requires an engine to pin (application-engine.h, "The records that cross").
static_assert(sizeof(ApplicationEngineEthereumAddress) == 20);
static_assert(sizeof(ApplicationEngineUint256) == 32);
static_assert(sizeof(ApplicationEngineByteSpan) == 16 && offsetof(ApplicationEngineByteSpan, size) == 8);
static_assert(sizeof(ApplicationEngineUserOp) == 24 && offsetof(ApplicationEngineUserOp, max_fee) == 4 &&
    offsetof(ApplicationEngineUserOp, data) == 8);
static_assert(sizeof(ApplicationEngineValidUserOp) == 40 && offsetof(ApplicationEngineValidUserOp, fee) == 20 &&
    offsetof(ApplicationEngineValidUserOp, data) == 24);
static_assert(sizeof(ApplicationEngineDirectInput) == 48 && offsetof(ApplicationEngineDirectInput, block_number) == 24 &&
    offsetof(ApplicationEngineDirectInput, payload) == 32);
static_assert(sizeof(ApplicationEngineProgress) == 16 && offsetof(ApplicationEngineProgress, last_executed_safe_block) == 8);
static_assert(sizeof(ApplicationEngineVoucher) == 72 && offsetof(ApplicationEngineVoucher, value) == 20 &&
    offsetof(ApplicationEngineVoucher, payload) == 56);
static_assert(sizeof(ApplicationEngineOutput) == 80 && offsetof(ApplicationEngineOutput, values) == 8);
static_assert(sizeof(ApplicationEngineInvalidNonce) == 8 && sizeof(ApplicationEngineInvalidMaxFee) == 4 &&
    sizeof(ApplicationEngineInsufficientFeeBalance) == 64);
static_assert(sizeof(ApplicationEngineInvalid) == 68 && offsetof(ApplicationEngineInvalid, values) == 4);
static_assert(sizeof(ApplicationEngineStatus) == sizeof(int32_t));
static_assert(sizeof(ApplicationEngineInvalidReason) == sizeof(int32_t));
static_assert(sizeof(ApplicationEngineOutputKind) == sizeof(int32_t));
static_assert(APPLICATION_ENGINE_ADDRESS_SIZE == sizeof(Address::b));
static_assert(APPLICATION_ENGINE_VALUE_SIZE == sizeof(U256::b));

namespace {

constexpr uint64_t MAX_METHOD_PAYLOAD_BYTES = 2048;

thread_local std::string t_error;
thread_local std::string t_path;
ApplicationEngine *g_live = nullptr; // the engine opened over machine ranges (seq_ext.h)

void set_error(const std::string &msg) {
    t_error = msg;
}

__attribute__((format(printf, 1, 2))) void logf(const char *fmt, ...) {
    const char *v = std::getenv("QA_LOG");
    if (v != nullptr && *v == '0') {
        return;
    }
    std::va_list ap;
    va_start(ap, fmt);
    std::fprintf(stderr, "[qa-wallet-engine] ");
    std::vfprintf(stderr, fmt, ap);
    std::fprintf(stderr, "\n");
    va_end(ap);
}

struct QueuedOutput {
    bool is_voucher{false};
    Address destination;
    U256 value;
    bytes payload;
};

} // namespace

// The engine instance behind the opaque handle.
struct ApplicationEngine final : OutputSink {
    Config cfg;
    std::unique_ptr<Engine> engine;
    std::string working_path; // private copy of the dump (host); empty for a live machine range
    std::deque<QueuedOutput> queue;
    std::optional<QueuedOutput> handed; // keeps the last drained payload alive until the next drain
    qawallet_aux_sink aux{};
    bool aux_set{false};
    qawallet_input_context ctx{};

    ~ApplicationEngine() override {
        if (g_live == this) {
            g_live = nullptr;
        }
        engine.reset();
        if (!working_path.empty()) {
            ::unlink(working_path.c_str());
        }
    }

    // OutputSink: notices and vouchers queue for drain_output; reports go to the aux sink.
    int notice(bytes_view payload) override {
        queue.push_back(QueuedOutput{false, Address{}, U256{}, bytes(payload.begin(), payload.end())});
        return 0;
    }
    int voucher(const Address &destination, const U256 &value, bytes_view payload) override {
        queue.push_back(QueuedOutput{true, destination, value, bytes(payload.begin(), payload.end())});
        return 0;
    }
    int report(bytes_view payload) override {
        if (aux_set && aux.report != nullptr) {
            return aux.report(aux.ctx, payload.data(), payload.size());
        }
        logf("report: %.*s", static_cast<int>(std::min<size_t>(payload.size(), 512)),
            reinterpret_cast<const char *>(payload.data()));
        return 0;
    }

    DirectInput make_input(const ApplicationEngineEthereumAddress &sender, uint64_t block, const ApplicationEngineByteSpan &data) {
        DirectInput in;
        std::copy(std::begin(sender.bytes), std::end(sender.bytes), in.sender.b.begin());
        in.block_number = block;
        in.payload = bytes_view(data.size == 0 ? nullptr : data.data, data.size);
        in.block_timestamp = ctx.block_timestamp;
        in.index = ctx.index;
        in.chain_id = ctx.chain_id;
        std::copy(std::begin(ctx.app_contract), std::end(ctx.app_contract), in.app_contract.b.begin());
        return in;
    }

    // Maps a completion to the seam's "included, maybe a no-op" outcome (docs/sequencer.md): a
    // rejected input keeps its reports, drops its queued outputs and is counted.
    void settle(Completion c, uint64_t block) {
        if (c == Completion::Reject) {
            queue.clear();
            engine->note_reject();
        }
        engine->commit_progress(block);
        engine->flush();
    }
};

namespace {

ApplicationEngineStatus from_open_error(const OpenError &e) {
    set_error(e.what());
    switch (e.kind) {
        case OpenErrorKind::NotFound:
            return APPLICATION_ENGINE_STATUS_NOT_FOUND;
        case OpenErrorKind::InvalidDump:
            return APPLICATION_ENGINE_STATUS_INVALID_DUMP;
        case OpenErrorKind::Io:
            return APPLICATION_ENGINE_STATUS_IO_ERROR;
    }
    return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
}

bool load_config(Config &cfg) {
    cfg = Config::devnet_defaults();
    std::string err;
    if (!cfg.apply_env(err)) {
        set_error("bad configuration: " + err);
        return false;
    }
    return true;
}

// Sparse-aware copy of a whole regular file; `dst_fd` ends with the size of `src_fd`.
bool copy_sparse(int src_fd, int dst_fd, std::string &err) {
    struct stat st{};
    if (::fstat(src_fd, &st) != 0) {
        err = std::string("fstat: ") + std::strerror(errno);
        return false;
    }
    off_t size = st.st_size;
    if (::ftruncate(dst_fd, size) != 0) {
        err = std::string("ftruncate: ") + std::strerror(errno);
        return false;
    }
    off_t pos = 0;
    while (pos < size) {
        off_t data = ::lseek(src_fd, pos, SEEK_DATA);
        if (data < 0) {
            if (errno == ENXIO) {
                break; // only a hole remains
            }
            err = std::string("lseek: ") + std::strerror(errno);
            return false;
        }
        off_t hole = ::lseek(src_fd, data, SEEK_HOLE);
        if (hole < 0) {
            err = std::string("lseek: ") + std::strerror(errno);
            return false;
        }
        off_t in = data;
        off_t out = data;
        while (in < hole) {
            ssize_t n = ::copy_file_range(src_fd, &in, dst_fd, &out, static_cast<size_t>(hole - in), 0);
            if (n < 0) {
                err = std::string("copy_file_range: ") + std::strerror(errno);
                return false;
            }
            if (n == 0) {
                break;
            }
        }
        pos = hole;
    }
    return true;
}

bool fsync_parent(const std::string &path, std::string &err) {
    std::string dir = path;
    size_t slash = dir.find_last_of('/');
    dir = slash == std::string::npos ? "." : (slash == 0 ? "/" : dir.substr(0, slash));
    int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0 || ::fsync(fd) != 0) {
        err = "fsync " + dir + ": " + std::strerror(errno);
        if (fd >= 0) {
            ::close(fd);
        }
        return false;
    }
    ::close(fd);
    return true;
}

std::string working_dir() {
    for (const char *name : {"QA_ENGINE_WORKDIR", "TMPDIR"}) {
        const char *v = std::getenv(name);
        if (v != nullptr && *v != '\0') {
            return v;
        }
    }
    return "/tmp";
}

void fill_invalid(ApplicationEngineInvalid *out, const UserOpInvalid &inv) {
    std::memset(out, 0, sizeof *out);
    if (inv.reason == UserOpInvalid::Reason::Nonce) {
        out->reason = APPLICATION_ENGINE_INVALID_NONCE;
        out->values.nonce.expected = inv.expected;
        out->values.nonce.got = inv.got;
    } else {
        out->reason = APPLICATION_ENGINE_INSUFFICIENT_FEE_BALANCE;
        std::copy(inv.required.b.begin(), inv.required.b.end(), std::begin(out->values.fee_balance.required.bytes));
        std::copy(inv.available.b.begin(), inv.available.b.end(), std::begin(out->values.fee_balance.available.bytes));
    }
}

} // namespace

extern "C" {

uint64_t application_engine_max_method_payload_bytes(void) noexcept {
    return MAX_METHOD_PAYLOAD_BYTES;
}

const char *application_engine_get_last_error_message(void) noexcept {
    return t_error.c_str();
}

ApplicationEngineStatus application_engine_from_dump(const char *prefix, ApplicationEngine **out_engine) noexcept try {
    t_error.clear();
    if (prefix == nullptr || out_engine == nullptr) {
        set_error("from_dump: null argument");
        return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
    }
    auto eng = std::make_unique<ApplicationEngine>();
    if (!load_config(eng->cfg)) {
        return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
    }
    std::string p = prefix;
    std::string state_path;
    bool live = false;
    struct stat st{};
    if (::stat(p.c_str(), &st) != 0) {
        set_error("dump not found: " + p);
        return APPLICATION_ENGINE_STATUS_NOT_FOUND;
    }
    if (S_ISCHR(st.st_mode)) {
        // a live machine: the NVRAM, worked on in place (the only mutable "dump")
        state_path = p;
        live = true;
    } else {
        if (!S_ISREG(st.st_mode)) {
            set_error("dump is not a regular file: " + p);
            return APPLICATION_ENGINE_STATUS_INVALID_DUMP;
        }
        if (static_cast<size_t>(st.st_size) != eng->cfg.state_length()) {
            set_error("dump has " + std::to_string(st.st_size) + " bytes, the configured state has " +
                std::to_string(eng->cfg.state_length()));
            return APPLICATION_ENGINE_STATUS_INVALID_DUMP;
        }
        // private working copy: the source stays immutable, two engines may open the same dump
        std::string tmpl = working_dir() + "/qa-wallet-engine-XXXXXX";
        std::string tmp(tmpl.begin(), tmpl.end());
        int dst = ::mkstemp(tmp.data());
        if (dst < 0) {
            set_error("cannot create a working copy in " + working_dir() + ": " + std::strerror(errno));
            return APPLICATION_ENGINE_STATUS_IO_ERROR;
        }
        eng->working_path = tmp;
        int src = ::open(p.c_str(), O_RDONLY);
        std::string err;
        bool ok = src >= 0 && copy_sparse(src, dst, err);
        if (src >= 0) {
            ::close(src);
        }
        ::close(dst);
        if (!ok) {
            set_error("cannot copy the dump: " + (src < 0 ? std::string(std::strerror(errno)) : err));
            return APPLICATION_ENGINE_STATUS_IO_ERROR;
        }
        state_path = eng->working_path;
    }
    try {
        eng->engine = std::make_unique<Engine>(eng->cfg, state_path);
    } catch (const OpenError &e) {
        return from_open_error(e);
    }
    *out_engine = eng.release();
    if (live) {
        g_live = *out_engine;
    }
    return APPLICATION_ENGINE_STATUS_OK;
} catch (const std::exception &e) {
    set_error(std::string("from_dump: ") + e.what());
    return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
} catch (...) {
    set_error("from_dump: unknown failure");
    return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
}

void application_engine_destroy(ApplicationEngine *engine) noexcept {
    delete engine;
}

ApplicationEngineStatus application_engine_validate_user_op(const ApplicationEngine *engine,
    const ApplicationEngineEthereumAddress *sender, const ApplicationEngineUserOp *user_op, uint16_t current_fee,
    ApplicationEngineInvalid *out_invalid) noexcept try {
    t_error.clear();
    if (engine == nullptr || sender == nullptr || user_op == nullptr || out_invalid == nullptr) {
        set_error("validate_user_op: null argument");
        return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
    }
    Address a;
    std::copy(std::begin(sender->bytes), std::end(sender->bytes), a.b.begin());
    UserOpInvalid inv;
    if (engine->engine->validate_user_op(a, user_op->nonce, current_fee, inv)) {
        return APPLICATION_ENGINE_STATUS_OK;
    }
    fill_invalid(out_invalid, inv);
    return APPLICATION_ENGINE_STATUS_INVALID;
} catch (const std::exception &e) {
    set_error(std::string("validate_user_op: ") + e.what());
    return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
} catch (...) {
    set_error("validate_user_op: unknown failure");
    return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
}

ApplicationEngineStatus application_engine_execute_valid_user_op(ApplicationEngine *engine,
    const ApplicationEngineValidUserOp *user_op, uint64_t safe_block, uint64_t *out_output_count) noexcept try {
    t_error.clear();
    if (engine == nullptr || user_op == nullptr || out_output_count == nullptr) {
        set_error("execute_valid_user_op: null argument");
        return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
    }
    if (!engine->queue.empty()) {
        set_error("execute_valid_user_op: outputs of the previous execution are still queued");
        return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
    }
    engine->handed.reset();
    DirectInput in = engine->make_input(user_op->sender, safe_block, user_op->data);
    Completion c = engine->engine->execute_user_op(in, user_op->fee, *engine);
    engine->settle(c, safe_block);
    *out_output_count = engine->queue.size();
    return APPLICATION_ENGINE_STATUS_OK;
} catch (const std::exception &e) {
    set_error(std::string("execute_valid_user_op: ") + e.what());
    return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
} catch (...) {
    set_error("execute_valid_user_op: unknown failure");
    return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
}

ApplicationEngineStatus application_engine_execute_direct_input(ApplicationEngine *engine,
    const ApplicationEngineDirectInput *input, uint64_t *out_output_count) noexcept try {
    t_error.clear();
    if (engine == nullptr || input == nullptr || out_output_count == nullptr) {
        set_error("execute_direct_input: null argument");
        return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
    }
    if (!engine->queue.empty()) {
        set_error("execute_direct_input: outputs of the previous execution are still queued");
        return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
    }
    engine->handed.reset();
    DirectInput in = engine->make_input(input->sender, input->block_number, input->payload);
    Completion c = engine->engine->execute_direct_input(in, *engine);
    engine->settle(c, input->block_number);
    *out_output_count = engine->queue.size();
    return APPLICATION_ENGINE_STATUS_OK;
} catch (const std::exception &e) {
    set_error(std::string("execute_direct_input: ") + e.what());
    return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
} catch (...) {
    set_error("execute_direct_input: unknown failure");
    return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
}

ApplicationEngineStatus application_engine_drain_output(ApplicationEngine *engine, ApplicationEngineOutput *out_output) noexcept try {
    t_error.clear();
    if (engine == nullptr || out_output == nullptr) {
        set_error("drain_output: null argument");
        return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
    }
    if (engine->queue.empty()) {
        set_error("drain_output: no output queued");
        return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
    }
    engine->handed = std::move(engine->queue.front());
    engine->queue.pop_front();
    const QueuedOutput &o = *engine->handed;
    std::memset(out_output, 0, sizeof *out_output);
    ApplicationEngineByteSpan span{o.payload.empty() ? nullptr : o.payload.data(), o.payload.size()};
    if (o.is_voucher) {
        out_output->kind = APPLICATION_ENGINE_OUTPUT_VOUCHER;
        std::copy(o.destination.b.begin(), o.destination.b.end(), std::begin(out_output->values.voucher.destination.bytes));
        std::copy(o.value.b.begin(), o.value.b.end(), std::begin(out_output->values.voucher.value.bytes));
        out_output->values.voucher.payload = span;
    } else {
        out_output->kind = APPLICATION_ENGINE_OUTPUT_NOTICE;
        out_output->values.notice = span;
    }
    return APPLICATION_ENGINE_STATUS_OK;
} catch (const std::exception &e) {
    set_error(std::string("drain_output: ") + e.what());
    return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
} catch (...) {
    set_error("drain_output: unknown failure");
    return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
}

void application_engine_progress(const ApplicationEngine *engine, ApplicationEngineProgress *out_progress) noexcept {
    if (engine == nullptr || out_progress == nullptr) {
        return;
    }
    Progress p = engine->engine->progress();
    out_progress->executed_input_count = p.executed_input_count;
    out_progress->last_executed_safe_block = p.last_block;
}

ApplicationEngineStatus application_engine_create_dump(ApplicationEngine *engine, const char *prefix) noexcept try {
    t_error.clear();
    if (engine == nullptr || prefix == nullptr) {
        set_error("create_dump: null argument");
        return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
    }
    if (!engine->queue.empty()) {
        set_error("create_dump: an execution's outputs are still queued (not quiescent)");
        return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
    }
    if (engine->working_path.empty()) {
        set_error("create_dump: a live machine range cannot be dumped from the guest");
        return APPLICATION_ENGINE_STATUS_IO_ERROR;
    }
    struct stat st{};
    if (::stat(prefix, &st) == 0) {
        set_error(std::string("create_dump: path exists: ") + prefix);
        return APPLICATION_ENGINE_STATUS_IO_ERROR;
    }
    engine->engine->flush();
    int src = ::open(engine->working_path.c_str(), O_RDONLY);
    if (src < 0) {
        set_error(std::string("create_dump: cannot open the working copy: ") + std::strerror(errno));
        return APPLICATION_ENGINE_STATUS_IO_ERROR;
    }
    int dst = ::open(prefix, O_RDWR | O_CREAT | O_EXCL, 0644);
    if (dst < 0) {
        ::close(src);
        set_error(std::string("create_dump: cannot create ") + prefix + ": " + std::strerror(errno));
        return errno == ENOENT ? APPLICATION_ENGINE_STATUS_NOT_FOUND : APPLICATION_ENGINE_STATUS_IO_ERROR;
    }
    std::string err;
    bool ok = copy_sparse(src, dst, err) && (::fsync(dst) == 0 || (err = std::strerror(errno), false));
    ::close(src);
    ::close(dst);
    if (!ok || !fsync_parent(prefix, err)) {
        ::unlink(prefix);
        set_error("create_dump: " + err);
        return APPLICATION_ENGINE_STATUS_IO_ERROR;
    }
    return APPLICATION_ENGINE_STATUS_OK;
} catch (const std::exception &e) {
    set_error(std::string("create_dump: ") + e.what());
    return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
} catch (...) {
    set_error("create_dump: unknown failure");
    return APPLICATION_ENGINE_STATUS_INTERNAL_ERROR;
}

const char *application_engine_state_file_in_dump(const char *prefix) noexcept try {
    t_error.clear();
    if (prefix == nullptr) {
        set_error("state_file_in_dump: null prefix");
        return nullptr;
    }
    t_path = prefix; // the dump is the state file itself
    return t_path.c_str();
} catch (...) {
    set_error("state_file_in_dump: out of memory");
    return nullptr;
}

// ---------------------------------------------------------------------------- extensions

int qawallet_genesis(const char *state_path, const uint8_t sequencer[20], const uint8_t token[20]) {
    t_error.clear();
    try {
        if (state_path == nullptr || sequencer == nullptr) {
            set_error("genesis: null argument");
            return -EINVAL;
        }
        Config cfg;
        if (!load_config(cfg)) {
            return -EINVAL;
        }
        Address seq;
        std::copy_n(sequencer, 20, seq.b.begin());
        if (token != nullptr) {
            Address t;
            std::copy_n(token, 20, t.b.begin());
            if (!t.is_zero()) {
                cfg.token = t;
            }
        }
        Engine::genesis(cfg, state_path, seq);
        return 0;
    } catch (const std::exception &e) {
        set_error(std::string("genesis: ") + e.what());
        return -EIO;
    } catch (...) {
        set_error("genesis: unknown failure");
        return -EIO;
    }
}

int qawallet_live_attach_aux(const qawallet_aux_sink *aux) {
    if (g_live == nullptr) {
        return -ENOENT;
    }
    if (aux == nullptr) {
        g_live->aux_set = false;
        return 0;
    }
    g_live->aux = *aux;
    g_live->aux_set = true;
    return 0;
}

int qawallet_live_set_input_context(const qawallet_input_context *context) {
    if (g_live == nullptr) {
        return -ENOENT;
    }
    g_live->ctx = context == nullptr ? qawallet_input_context{} : *context;
    return 0;
}

int qawallet_live_set_scheduler_status(uint64_t next_batch_nonce, uint64_t queued_directs) {
    if (g_live == nullptr) {
        return -ENOENT;
    }
    g_live->engine->set_scheduler_status(next_batch_nonce, queued_directs);
    return 0;
}

int qawallet_live_inspect(const uint8_t *query, uint64_t size,
    int (*report)(void *ctx, const uint8_t *data, uint64_t size), void *ctx) {
    t_error.clear();
    if (g_live == nullptr) {
        return -ENOENT;
    }
    if (report == nullptr) {
        return -EINVAL;
    }
    try {
        struct InspectSink final : OutputSink {
            int (*cb)(void *, const uint8_t *, uint64_t);
            void *cb_ctx;
            int notice(bytes_view) override { return 0; }
            int voucher(const Address &, const U256 &, bytes_view) override { return 0; }
            int report(bytes_view p) override { return cb(cb_ctx, p.data(), p.size()); }
        } sink{};
        sink.cb = report;
        sink.cb_ctx = ctx;
        (void) g_live->engine->inspect(bytes_view(size == 0 ? nullptr : query, size), sink); // a reject wrote its report
        return 0;
    } catch (const std::exception &e) {
        set_error(std::string("inspect: ") + e.what());
        return -EIO;
    } catch (...) {
        set_error("inspect: unknown failure");
        return -EIO;
    }
}

void qawallet_set_live_engine(ApplicationEngine *engine) {
    g_live = engine;
}

int qawallet_fee_to_linear(uint16_t exponent, uint8_t out[32]) {
    U256 v;
    if (out == nullptr || !fee::to_linear(exponent, v)) {
        return -1;
    }
    std::copy(v.b.begin(), v.b.end(), out);
    return 0;
}

uint16_t qawallet_fee_max_exponent(void) {
    return fee::max_exponent();
}

} // extern "C"
