// Engine: turns a direct input (or inspect query) into ledger changes, emissions and a completion.
#include "qawallet/engine.h"

#include "qawallet/fee.h"
#include "qawallet/state.h"

extern "C" {
#include <libcma/parser.h>
#include <libcmt/abi.h>
#include <libcmt/keccak.h>
#include <libcmt/rollup.h>
}

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

namespace qawallet {

using json = nlohmann::json;

namespace {

constexpr size_t MAX_INSPECT_JSON = 1u << 20; // JSON inspects larger than this are treated as raw echo

[[nodiscard]] std::string dec(const U256 &v) {
    return u256_to_dec(v);
}

[[nodiscard]] U256 from_cma(const cmt_abi_u256_t &a) {
    U256 v;
    std::copy(std::begin(a.data), std::end(a.data), v.b.begin());
    return v;
}

[[nodiscard]] Address from_cma(const cmt_abi_address_t &a) {
    Address v;
    std::copy(std::begin(a.data), std::end(a.data), v.b.begin());
    return v;
}

[[nodiscard]] bytes_view view(const cmt_abi_bytes_t &b) {
    return bytes_view(static_cast<const uint8_t *>(b.data), b.data == nullptr ? 0 : b.length);
}

[[nodiscard]] std::string keccak_hex(bytes_view data) {
    uint8_t md[CMT_KECCAK_LENGTH];
    cmt_keccak_t st;
    cmt_keccak_init(&st);
    cmt_keccak_update(&st, data.size(), data.data());
    cmt_keccak_final(&st, md);
    return to_hex(bytes_view(md, sizeof md));
}

} // namespace

bool is_libcma_selector(bytes_view payload) {
    if (payload.size() < 4) {
        return false;
    }
    const uint32_t sel = (static_cast<uint32_t>(payload[0]) << 24) | (static_cast<uint32_t>(payload[1]) << 16) |
        (static_cast<uint32_t>(payload[2]) << 8) | static_cast<uint32_t>(payload[3]);
    switch (sel) {
        case WITHDRAW_ETHER:
        case WITHDRAW_ERC20:
        case WITHDRAW_ERC721:
        case WITHDRAW_ERC1155_SINGLE:
        case WITHDRAW_ERC1155_BATCH:
        case TRANSFER_ETHER:
        case TRANSFER_ERC20:
        case TRANSFER_ERC721:
        case TRANSFER_ERC1155_SINGLE:
        case TRANSFER_ERC1155_BATCH:
            return true;
        default:
            return false;
    }
}

struct Engine::Impl {
    Config cfg;
    Ledger ledger;
    MetaView meta;
    NonceTable nonces;
    bool scheduler_known{false};
    uint64_t scheduler_next_nonce{0};
    uint64_t scheduler_queued{0};

    explicit Impl(Config c, const std::string &state_file)
        : cfg(c), ledger(c, state_file), meta{ledger.region_c()},
          nonces(ledger.region_c() + META_PAGE_LENGTH, c.region_c_length - META_PAGE_LENGTH, meta) {}

    __attribute__((format(printf, 2, 3))) void log(const char *fmt, ...) {
        if (!cfg.log) {
            return;
        }
        std::va_list ap;
        va_start(ap, fmt);
        std::fprintf(stderr, "[qa-wallet] ");
        std::vfprintf(stderr, fmt, ap);
        std::fprintf(stderr, "\n");
        va_end(ap);
    }

    // --- reporting helpers -------------------------------------------------------------------
    int report_json(OutputSink &sink, const json &j) {
        std::string s = j.dump();
        return sink.report(bytes_view(reinterpret_cast<const uint8_t *>(s.data()), s.size()));
    }

    Completion fail(OutputSink &sink, const std::string &what, int code = 0) {
        log("reject: %s (%d)", what.c_str(), code);
        json j{{"error", what}};
        if (code != 0) {
            j["code"] = code;
        }
        (void) report_json(sink, j);
        return Completion::Reject;
    }

    Completion emit_check(OutputSink &sink, int rc, const char *what) {
        if (rc < 0) {
            return fail(sink, std::string("unable to emit ") + what, rc);
        }
        return Completion::Accept;
    }

    // --- portal deposits ---------------------------------------------------------------------
    [[nodiscard]] cma_parser_input_type_t portal_kind(const Address &sender) const {
        if (sender == cfg.ether_portal) {
            return CMA_PARSER_INPUT_TYPE_ETHER_DEPOSIT;
        }
        if (sender == cfg.erc20_portal) {
            return CMA_PARSER_INPUT_TYPE_ERC20_DEPOSIT;
        }
        if (sender == cfg.erc721_portal) {
            return CMA_PARSER_INPUT_TYPE_ERC721_DEPOSIT;
        }
        if (sender == cfg.erc1155_single_portal) {
            return CMA_PARSER_INPUT_TYPE_ERC1155_SINGLE_DEPOSIT;
        }
        if (sender == cfg.erc1155_batch_portal) {
            return CMA_PARSER_INPUT_TYPE_ERC1155_BATCH_DEPOSIT;
        }
        return CMA_PARSER_INPUT_TYPE_NONE;
    }

    static cmt_rollup_advance_t to_cmt(const DirectInput &in) {
        cmt_rollup_advance_t a{};
        a.chain_id = in.chain_id;
        std::copy(in.app_contract.b.begin(), in.app_contract.b.end(), std::begin(a.app_contract.data));
        std::copy(in.sender.b.begin(), in.sender.b.end(), std::begin(a.msg_sender.data));
        a.block_number = in.block_number;
        a.block_timestamp = in.block_timestamp;
        a.index = in.index;
        a.payload.length = in.payload.size();
        a.payload.data = const_cast<uint8_t *>(in.payload.data());
        return a;
    }

    Completion handle_deposit(const DirectInput &in, cma_parser_input_type_t kind, OutputSink &sink) {
        cmt_rollup_advance_t adv = to_cmt(in);
        cma_parser_input_t p{};
        int rc = cma_parser_decode_advance(kind, &adv, &p);
        if (rc != CMA_PARSER_SUCCESS) {
            return fail(sink, std::string("unable to decode deposit: ") + cma_parser_get_last_error_message(), rc);
        }
        json rep;
        Address depositor;
        AssetRef asset;
        std::vector<std::pair<AssetRef, U256>> credits;
        switch (p.type) {
            case CMA_PARSER_INPUT_TYPE_ETHER_DEPOSIT:
                depositor = from_cma(p.ether_deposit.sender);
                asset = AssetRef::ether();
                credits.emplace_back(asset, from_cma(p.ether_deposit.amount));
                rep = {{"deposit", "ether"}, {"sender", to_hex(depositor)}, {"amount", dec(from_cma(p.ether_deposit.amount))},
                    {"exec_layer_data", to_hex(view(p.ether_deposit.exec_layer_data))}};
                break;
            case CMA_PARSER_INPUT_TYPE_ERC20_DEPOSIT:
                depositor = from_cma(p.erc20_deposit.sender);
                asset = AssetRef::erc20(from_cma(p.erc20_deposit.token));
                credits.emplace_back(asset, from_cma(p.erc20_deposit.amount));
                rep = {{"deposit", "erc20"}, {"sender", to_hex(depositor)}, {"token", to_hex(asset.token)},
                    {"amount", dec(from_cma(p.erc20_deposit.amount))},
                    {"exec_layer_data", to_hex(view(p.erc20_deposit.exec_layer_data))}};
                break;
            case CMA_PARSER_INPUT_TYPE_ERC721_DEPOSIT:
                depositor = from_cma(p.erc721_deposit.sender);
                asset = AssetRef::erc721(from_cma(p.erc721_deposit.token), from_cma(p.erc721_deposit.token_id));
                credits.emplace_back(asset, U256::from_u64(1));
                rep = {{"deposit", "erc721"}, {"sender", to_hex(depositor)}, {"token", to_hex(asset.token)},
                    {"token_id", dec(asset.id)}, {"base_layer_data", to_hex(view(p.erc721_deposit.base_layer_data))},
                    {"exec_layer_data", to_hex(view(p.erc721_deposit.exec_layer_data))}};
                break;
            case CMA_PARSER_INPUT_TYPE_ERC1155_SINGLE_DEPOSIT:
                depositor = from_cma(p.erc1155_single_deposit.sender);
                asset = AssetRef::erc1155(from_cma(p.erc1155_single_deposit.token),
                    from_cma(p.erc1155_single_deposit.token_id));
                credits.emplace_back(asset, from_cma(p.erc1155_single_deposit.amount));
                rep = {{"deposit", "erc1155_single"}, {"sender", to_hex(depositor)}, {"token", to_hex(asset.token)},
                    {"token_id", dec(asset.id)}, {"amount", dec(from_cma(p.erc1155_single_deposit.amount))},
                    {"base_layer_data", to_hex(view(p.erc1155_single_deposit.base_layer_data))},
                    {"exec_layer_data", to_hex(view(p.erc1155_single_deposit.exec_layer_data))}};
                break;
            case CMA_PARSER_INPUT_TYPE_ERC1155_BATCH_DEPOSIT: {
                depositor = from_cma(p.erc1155_batch_deposit.sender);
                Address token = from_cma(p.erc1155_batch_deposit.token);
                json ids = json::array();
                json amounts = json::array();
                if (p.erc1155_batch_deposit.token_ids.length != p.erc1155_batch_deposit.amounts.length) {
                    return fail(sink, "erc1155 batch: ids/amounts length mismatch");
                }
                for (size_t t = 0; t < p.erc1155_batch_deposit.token_ids.length; ++t) {
                    U256 id;
                    U256 amount;
                    std::copy_n(p.erc1155_batch_deposit.token_ids.data[t], 32, id.b.begin());
                    std::copy_n(p.erc1155_batch_deposit.amounts.data[t], 32, amount.b.begin());
                    credits.emplace_back(AssetRef::erc1155(token, id), amount);
                    ids.push_back(dec(id));
                    amounts.push_back(dec(amount));
                }
                asset = AssetRef::erc1155(token, U256{});
                rep = {{"deposit", "erc1155_batch"}, {"sender", to_hex(depositor)}, {"token", to_hex(token)},
                    {"token_ids", ids}, {"amounts", amounts},
                    {"base_layer_data", to_hex(view(p.erc1155_batch_deposit.base_layer_data))},
                    {"exec_layer_data", to_hex(view(p.erc1155_batch_deposit.exec_layer_data))}};
                break;
            }
            default:
                return fail(sink, "unexpected parser type for deposit");
        }
        rep["accounts_drive"] = ledger.is_accounts_drive_asset(asset);
        AccountRef acc = AccountRef::of_wallet(depositor);
        for (auto &[a, amount] : credits) {
            int lrc = ledger.deposit(acc, a, amount);
            if (lrc != 0) {
                rep["credited"] = false;
                rep["error"] = ledger.last_error();
                rep["code"] = lrc;
                (void) report_json(sink, rep);
                log("deposit failed: %s", ledger.last_error().c_str());
                return Completion::Reject;
            }
        }
        rep["credited"] = true;
        if (int rc2 = report_json(sink, rep); rc2 < 0) {
            return fail(sink, "unable to emit deposit report", rc2);
        }
        return Completion::Accept;
    }

    // --- libcma withdrawals / transfers ------------------------------------------------------
    Completion emit_cma_voucher(OutputSink &sink, cma_parser_voucher_type_t type, const Address &app,
        cma_parser_voucher_data_t &req, size_t payload_size) {
        bytes payload(payload_size);
        cma_voucher_t v{};
        v.payload.length = payload_size;
        v.payload.data = payload_size ? payload.data() : nullptr;
        cma_abi_address_t app_c{};
        std::copy(app.b.begin(), app.b.end(), std::begin(app_c.data));
        int rc = cma_parser_encode_voucher(type, &app_c, &req, &v);
        if (rc < 0) {
            return fail(sink, std::string("unable to encode voucher: ") + cma_parser_get_last_error_message(), rc);
        }
        rc = sink.voucher(from_cma(v.address), from_cma(v.value), view(v.payload));
        if (rc < 0) {
            return fail(sink, "unable to emit voucher", rc);
        }
        return Completion::Accept;
    }

    Completion handle_cma_advance(const DirectInput &in, OutputSink &sink) {
        cmt_rollup_advance_t adv = to_cmt(in);
        cma_parser_input_t p{};
        int rc = cma_parser_decode_advance(CMA_PARSER_INPUT_TYPE_AUTO, &adv, &p);
        if (rc != CMA_PARSER_SUCCESS) {
            return fail(sink, std::string("malformed ledger input: ") + cma_parser_get_last_error_message(), rc);
        }
        AccountRef me = AccountRef::of_wallet(in.sender);
        cma_parser_voucher_data_t req{};
        std::copy(in.sender.b.begin(), in.sender.b.end(), std::begin(req.receiver.data));
        auto withdraw = [&](const AssetRef &asset, const U256 &amount) -> bool {
            int lrc = ledger.withdraw(me, asset, amount);
            if (lrc != 0) {
                (void) fail(sink, ledger.last_error(), lrc);
                return false;
            }
            return true;
        };
        switch (p.type) {
            case CMA_PARSER_INPUT_TYPE_ETHER_WITHDRAWAL: {
                U256 amount = from_cma(p.ether_withdrawal.amount);
                if (!withdraw(AssetRef::ether(), amount)) {
                    return Completion::Reject;
                }
                req.ether.amount = p.ether_withdrawal.amount;
                return emit_cma_voucher(sink, CMA_PARSER_VOUCHER_TYPE_ETHER, in.app_contract, req, 0);
            }
            case CMA_PARSER_INPUT_TYPE_ERC20_WITHDRAWAL: {
                AssetRef asset = AssetRef::erc20(from_cma(p.erc20_withdrawal.token));
                if (!withdraw(asset, from_cma(p.erc20_withdrawal.amount))) {
                    return Completion::Reject;
                }
                req.erc20.token = p.erc20_withdrawal.token;
                req.erc20.amount = p.erc20_withdrawal.amount;
                return emit_cma_voucher(sink, CMA_PARSER_VOUCHER_TYPE_ERC20, in.app_contract, req,
                    CMA_PARSER_ERC20_VOUCHER_PAYLOAD_SIZE);
            }
            case CMA_PARSER_INPUT_TYPE_ERC721_WITHDRAWAL: {
                AssetRef asset = AssetRef::erc721(from_cma(p.erc721_withdrawal.token), from_cma(p.erc721_withdrawal.token_id));
                if (!withdraw(asset, U256::from_u64(1))) {
                    return Completion::Reject;
                }
                req.erc721.token = p.erc721_withdrawal.token;
                req.erc721.token_id = p.erc721_withdrawal.token_id;
                return emit_cma_voucher(sink, CMA_PARSER_VOUCHER_TYPE_ERC721, in.app_contract, req,
                    CMA_PARSER_ERC721_VOUCHER_PAYLOAD_SIZE);
            }
            case CMA_PARSER_INPUT_TYPE_ERC1155_SINGLE_WITHDRAWAL: {
                AssetRef asset = AssetRef::erc1155(from_cma(p.erc1155_single_withdrawal.token),
                    from_cma(p.erc1155_single_withdrawal.token_id));
                if (!withdraw(asset, from_cma(p.erc1155_single_withdrawal.amount))) {
                    return Completion::Reject;
                }
                req.erc1155_single.token = p.erc1155_single_withdrawal.token;
                req.erc1155_single.token_id = p.erc1155_single_withdrawal.token_id;
                req.erc1155_single.amount = p.erc1155_single_withdrawal.amount;
                return emit_cma_voucher(sink, CMA_PARSER_VOUCHER_TYPE_ERC1155_SINGLE, in.app_contract, req,
                    CMA_PARSER_ERC1155_SINGLE_VOUCHER_PAYLOAD_MIN_SIZE);
            }
            case CMA_PARSER_INPUT_TYPE_ERC1155_BATCH_WITHDRAWAL: {
                auto &w = p.erc1155_batch_withdrawal;
                if (w.token_ids.length != w.amounts.length) {
                    return fail(sink, "erc1155 batch: ids/amounts length mismatch");
                }
                Address token = from_cma(w.token);
                for (size_t t = 0; t < w.token_ids.length; ++t) {
                    U256 id;
                    U256 amount;
                    std::copy_n(w.token_ids.data[t], 32, id.b.begin());
                    std::copy_n(w.amounts.data[t], 32, amount.b.begin());
                    if (!withdraw(AssetRef::erc1155(token, id), amount)) {
                        return Completion::Reject;
                    }
                }
                req.erc1155_batch.token = w.token;
                req.erc1155_batch.token_ids = w.token_ids;
                req.erc1155_batch.amounts = w.amounts;
                size_t size = CMA_PARSER_ERC1155_BATCH_VOUCHER_PAYLOAD_MIN_SIZE + 2 * w.token_ids.length * CMA_ABI_U256_LENGTH;
                return emit_cma_voucher(sink, CMA_PARSER_VOUCHER_TYPE_ERC1155_BATCH, in.app_contract, req, size);
            }
            case CMA_PARSER_INPUT_TYPE_ETHER_TRANSFER:
                return do_transfer(sink, me, from_cma(p.ether_transfer.receiver), AssetRef::ether(), from_cma(p.ether_transfer.amount));
            case CMA_PARSER_INPUT_TYPE_ERC20_TRANSFER:
                return do_transfer(sink, me, from_cma(p.erc20_transfer.receiver), AssetRef::erc20(from_cma(p.erc20_transfer.token)),
                    from_cma(p.erc20_transfer.amount));
            case CMA_PARSER_INPUT_TYPE_ERC721_TRANSFER:
                return do_transfer(sink, me, from_cma(p.erc721_transfer.receiver),
                    AssetRef::erc721(from_cma(p.erc721_transfer.token), from_cma(p.erc721_transfer.token_id)), U256::from_u64(1));
            case CMA_PARSER_INPUT_TYPE_ERC1155_SINGLE_TRANSFER:
                return do_transfer(sink, me, from_cma(p.erc1155_single_transfer.receiver),
                    AssetRef::erc1155(from_cma(p.erc1155_single_transfer.token), from_cma(p.erc1155_single_transfer.token_id)),
                    from_cma(p.erc1155_single_transfer.amount));
            case CMA_PARSER_INPUT_TYPE_ERC1155_BATCH_TRANSFER: {
                auto &t = p.erc1155_batch_transfer;
                if (t.token_ids.length != t.amounts.length) {
                    return fail(sink, "erc1155 batch: ids/amounts length mismatch");
                }
                Address token = from_cma(t.token);
                for (size_t i = 0; i < t.token_ids.length; ++i) {
                    U256 id;
                    U256 amount;
                    std::copy_n(t.token_ids.data[i], 32, id.b.begin());
                    std::copy_n(t.amounts.data[i], 32, amount.b.begin());
                    Completion c = do_transfer(sink, me, from_cma(t.receiver), AssetRef::erc1155(token, id), amount);
                    if (c != Completion::Accept) {
                        return c;
                    }
                }
                return Completion::Accept;
            }
            default:
                return fail(sink, "unsupported libcma input type", static_cast<int>(p.type));
        }
    }

    Completion do_transfer(OutputSink &sink, const AccountRef &from, const U256 &to_id, const AssetRef &asset, const U256 &amount) {
        int lrc = ledger.transfer(from, AccountRef::of_id(to_id), asset, amount);
        if (lrc != 0) {
            return fail(sink, ledger.last_error(), lrc);
        }
        json rep{{"transfer", "ok"}, {"to", to_hex(to_id)}, {"amount", dec(amount)}};
        if (asset.kind != AssetKind::Base) {
            rep["token"] = to_hex(asset.token);
        }
        (void) report_json(sink, rep);
        return Completion::Accept;
    }

    // --- the dispatch ------------------------------------------------------------------------
    Completion run_payload(const DirectInput &in, OutputSink &sink) {
        if (is_libcma_selector(in.payload)) {
            return handle_cma_advance(in, sink);
        }
        log("echo (%zu bytes)", in.payload.size());
        return emit_check(sink, sink.notice(in.payload), "echo notice");
    }

    // --- inspect ------------------------------------------------------------------------------
    json state_json() {
        bytes_view drive = ledger.accounts_drive();
        json j{{"executed_input_count", meta.executed_input_count()},
            {"last_block", meta.last_executed_safe_block()}, {"rejected_count", meta.rejected_count()},
            {"sequencer_address", to_hex(meta.sequencer())}, {"token", to_hex(meta.token())},
            {"nonce_count", meta.nonce_count()},
            {"state_backend", ledger.is_nvram() ? "nvram" : "file"}, {"accounts_drive_length", drive.size()},
            {"accounts_drive_keccak256", drive.empty() ? std::string("unavailable") : keccak_hex(drive)}};
        if (scheduler_known) {
            j["scheduler"] = json{{"next_batch_nonce", scheduler_next_nonce}, {"queued_directs", scheduler_queued}};
        }
        return j;
    }

    Completion handle_inspect(bytes_view payload, OutputSink &sink) {
        static const std::string STATE = "state";
        bool is_json = !payload.empty() && payload[0] == '{' && payload.size() <= MAX_INSPECT_JSON;
        if (!is_json) {
            if (payload.size() == STATE.size() && std::equal(payload.begin(), payload.end(), STATE.begin())) {
                return emit_check(sink, report_json(sink, state_json()), "state report");
            }
            // anything that is not a JSON query is echoed back as one report
            return emit_check(sink, sink.report(payload), "echo report");
        }
        json q;
        try {
            q = json::parse(payload.begin(), payload.end());
        } catch (const std::exception &e) {
            return fail(sink, std::string("invalid json: ") + e.what());
        }
        std::string method = q.value("method", "");
        json params = q.contains("params") ? q["params"] : json::array();
        auto param_u64 = [&](size_t i, uint64_t def) -> uint64_t {
            if (!params.is_array() || params.size() <= i) {
                return def;
            }
            if (params[i].is_number_unsigned()) {
                return params[i].get<uint64_t>();
            }
            if (params[i].is_string()) {
                U256 v;
                if (parse_u256(params[i].get<std::string>(), v) && v.fits_u64()) {
                    return v.to_u64();
                }
            }
            return def;
        };
        if (method == "ledger_getNonce") {
            Address a;
            if (!params.is_array() || params.empty() || !params[0].is_string() ||
                !parse_address(params[0].get<std::string>(), a)) {
                return fail(sink, "ledger_getNonce: params[0] must be an address");
            }
            U256 out = U256::from_u64(nonces.get(a));
            return emit_check(sink, sink.report(bytes_view(out.b.data(), out.b.size())), "nonce report");
        }
        if (method == "qa_fee") {
            uint64_t e = param_u64(0, 0);
            U256 out;
            if (e > 0xffffU || !fee::to_linear(static_cast<uint16_t>(e), out)) {
                return fail(sink, "qa_fee: exponent above the representable maximum " +
                    std::to_string(fee::max_exponent()));
            }
            return emit_check(sink, sink.report(bytes_view(out.b.data(), out.b.size())), "fee report");
        }
        // libcma inspects: ledger_getBalance / ledger_getTotalSupply
        cmt_rollup_inspect_t ins{};
        ins.payload.length = payload.size();
        ins.payload.data = const_cast<uint8_t *>(payload.data());
        cma_parser_input_t p{};
        int rc = cma_parser_decode_inspect(CMA_PARSER_INPUT_TYPE_AUTO, &ins, &p);
        if (rc != CMA_PARSER_SUCCESS) {
            return fail(sink, std::string("unknown inspect: ") + cma_parser_get_last_error_message(), rc);
        }
        U256 out;
        int lrc = 0;
        switch (p.type) {
            case CMA_PARSER_INPUT_TYPE_BALANCE_ACCOUNT:
                lrc = ledger.balance(AccountRef::of_id(from_cma(p.balance.account)), AssetRef::ether(), out);
                break;
            case CMA_PARSER_INPUT_TYPE_BALANCE_ACCOUNT_TOKEN_ADDRESS:
                lrc = ledger.balance(AccountRef::of_id(from_cma(p.balance.account)), AssetRef::erc20(from_cma(p.balance.token)), out);
                break;
            case CMA_PARSER_INPUT_TYPE_BALANCE_ACCOUNT_TOKEN_ADDRESS_ID: {
                AssetRef a = AssetRef::erc721(from_cma(p.balance.token), from_cma(p.balance.token_id));
                lrc = ledger.balance(AccountRef::of_id(from_cma(p.balance.account)), a, out);
                if (lrc != 0) { // maybe an ERC-1155 asset
                    a.kind = AssetKind::TokenIdAmount;
                    lrc = ledger.balance(AccountRef::of_id(from_cma(p.balance.account)), a, out);
                }
                break;
            }
            case CMA_PARSER_INPUT_TYPE_SUPPLY:
                lrc = ledger.total_supply(AssetRef::ether(), out);
                break;
            case CMA_PARSER_INPUT_TYPE_SUPPLY_TOKEN_ADDRESS:
                lrc = ledger.total_supply(AssetRef::erc20(from_cma(p.supply.token)), out);
                break;
            case CMA_PARSER_INPUT_TYPE_SUPPLY_TOKEN_ADDRESS_ID: {
                AssetRef a = AssetRef::erc721(from_cma(p.supply.token), from_cma(p.supply.token_id));
                lrc = ledger.total_supply(a, out);
                if (lrc != 0) {
                    a.kind = AssetKind::TokenIdAmount;
                    lrc = ledger.total_supply(a, out);
                }
                break;
            }
            default:
                return fail(sink, "unsupported inspect type", static_cast<int>(p.type));
        }
        if (lrc != 0) {
            return fail(sink, ledger.last_error(), lrc);
        }
        return emit_check(sink, sink.report(bytes_view(out.b.data(), out.b.size())), "balance report");
    }
};

namespace {

// Maps the state range just long enough to look at the meta page (a UIO range is always mapped
// whole, at offset 0). `token` receives the pinned asset when the magic is present.
bool peek_initialized(const Config &cfg, const std::string &state_path, Address *token = nullptr) {
    struct stat st{};
    if (::stat(state_path.c_str(), &st) != 0) {
        throw OpenError(OpenErrorKind::NotFound, "state not found: " + state_path);
    }
    if (S_ISREG(st.st_mode) && static_cast<size_t>(st.st_size) < cfg.state_length()) {
        return false; // too short to hold a genesis
    }
    int fd = ::open(state_path.c_str(), O_RDWR);
    if (fd < 0) {
        throw OpenError(OpenErrorKind::Io, "cannot open " + state_path + ": " + std::strerror(errno));
    }
    void *p = ::mmap(nullptr, cfg.state_length(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    ::close(fd);
    if (p == MAP_FAILED) {
        throw OpenError(OpenErrorKind::Io, "cannot map " + state_path + ": " + std::strerror(errno));
    }
    MetaView meta{static_cast<uint8_t *>(p) + cfg.region_c_offset()};
    bool ok = meta.has_magic();
    if (ok && token != nullptr) {
        *token = meta.token();
    }
    ::munmap(p, cfg.state_length());
    return ok;
}

} // namespace

bool Engine::is_initialized(const Config &cfg, const std::string &state_path) {
    return peek_initialized(cfg, state_path);
}

void Engine::genesis(const Config &cfg, const std::string &state_path, const Address &sequencer) {
    struct stat st{};
    if (::stat(state_path.c_str(), &st) != 0) {
        // a regular file is created; devices must exist
        int fd = ::open(state_path.c_str(), O_RDWR | O_CREAT | O_EXCL, 0644);
        if (fd < 0) {
            throw std::runtime_error("cannot create " + state_path + ": " + std::strerror(errno));
        }
        ::close(fd);
    } else if (S_ISCHR(st.st_mode)) {
        // A live UIO range: region B can be laid out exactly once per process (ledger.h), so the
        // genesis only marks the meta page here and the first open lays out both ledgers. The
        // bytes are the same as a file genesis: libcma's creation does not depend on the order.
        int fd = ::open(state_path.c_str(), O_RDWR);
        if (fd < 0) {
            throw std::runtime_error("cannot open " + state_path + ": " + std::strerror(errno));
        }
        void *p = ::mmap(nullptr, cfg.state_length(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        ::close(fd);
        if (p == MAP_FAILED) {
            throw std::runtime_error("cannot map " + state_path + ": " + std::strerror(errno));
        }
        MetaView meta{static_cast<uint8_t *>(p) + cfg.region_c_offset()};
        if (meta.has_magic()) {
            ::munmap(p, cfg.state_length());
            throw std::runtime_error("state already carries a genesis: " + state_path);
        }
        meta.initialize(sequencer, cfg.token);
        ::munmap(p, cfg.state_length());
        return;
    }
    Ledger ledger(cfg, state_path); // grows the file, creates both libcma ledgers
    MetaView meta{ledger.region_c()};
    if (meta.has_magic()) {
        throw std::runtime_error("state already carries a genesis: " + state_path);
    }
    meta.initialize(sequencer, cfg.token);
    ledger.flush();
}

Engine::Engine(Config cfg, const std::string &state_file) {
    Address pinned;
    if (!peek_initialized(cfg, state_file, &pinned)) {
        throw OpenError(OpenErrorKind::InvalidDump, "state carries no genesis (meta page magic missing): " + state_file);
    }
    cfg.token = pinned; // the single-asset ledger's asset is whatever the genesis pinned
    try {
        m = std::make_unique<Impl>(cfg, state_file);
    } catch (const std::runtime_error &e) {
        throw OpenError(OpenErrorKind::InvalidDump, e.what());
    }
}
Engine::~Engine() = default;

Completion Engine::execute_direct_input(const DirectInput &input, OutputSink &sink) {
    Completion c = Completion::Reject;
    try {
        cma_parser_input_type_t kind = m->portal_kind(input.sender);
        if (kind != CMA_PARSER_INPUT_TYPE_NONE) {
            c = m->handle_deposit(input, kind, sink);
        } else {
            c = m->run_payload(input, sink);
        }
    } catch (const std::exception &e) {
        c = m->fail(sink, std::string("internal error: ") + e.what());
    } catch (...) {
        c = m->fail(sink, "internal error");
    }
    return c;
}

bool Engine::validate_user_op(const Address &sender, uint32_t nonce, uint16_t current_fee, UserOpInvalid &invalid) const {
    invalid = UserOpInvalid{};
    uint32_t expected = m->nonces.get(sender);
    if (nonce != expected) {
        invalid.reason = UserOpInvalid::Reason::Nonce;
        invalid.expected = expected;
        invalid.got = nonce;
        return false;
    }
    U256 required;
    bool representable = fee::to_linear(current_fee, required);
    U256 available;
    (void) m->ledger.balance(AccountRef::of_wallet(sender), AssetRef::erc20(m->cfg.token), available); // absent = 0
    // a sender the nonce table cannot take is refused like an uncoverable fee (documented)
    if (!representable || !m->nonces.can_insert(sender)) {
        invalid.reason = UserOpInvalid::Reason::FeeBalance;
        std::fill(invalid.required.b.begin(), invalid.required.b.end(), uint8_t{0xff});
        invalid.available = available;
        return false;
    }
    if (std::lexicographical_compare(available.b.begin(), available.b.end(), required.b.begin(), required.b.end())) {
        invalid.reason = UserOpInvalid::Reason::FeeBalance;
        invalid.required = required;
        invalid.available = available;
        return false;
    }
    return true;
}

Completion Engine::execute_user_op(const DirectInput &in, uint16_t fee, OutputSink &sink) {
    U256 amount;
    if (!fee::to_linear(fee, amount)) {
        throw std::runtime_error("user op fee exponent is not representable");
    }
    Address seq = m->meta.sequencer();
    if (!amount.is_zero() && !(in.sender == seq)) {
        int rc = m->ledger.transfer(AccountRef::of_wallet(in.sender), AccountRef::of_wallet(seq),
            AssetRef::erc20(m->cfg.token), amount);
        if (rc != 0) {
            throw std::runtime_error("validated user op cannot pay fee: " + m->ledger.last_error());
        }
    }
    uint32_t nonce = m->nonces.get(in.sender);
    if (nonce == UINT32_MAX || !m->nonces.set(in.sender, nonce + 1)) {
        throw std::runtime_error("user op nonce cannot advance");
    }
    Completion c = Completion::Reject;
    try {
        c = m->run_payload(in, sink);
    } catch (const std::exception &e) {
        c = m->fail(sink, std::string("internal error: ") + e.what());
    } catch (...) {
        c = m->fail(sink, "internal error");
    }
    return c;
}

void Engine::commit_progress(uint64_t block) {
    uint64_t count = m->meta.executed_input_count();
    if (count == UINT64_MAX) {
        throw std::runtime_error("executed input count overflow");
    }
    m->meta.set_progress(count + 1, std::max(m->meta.last_executed_safe_block(), block));
}

void Engine::note_reject() {
    m->meta.set_rejected_count(m->meta.rejected_count() + 1);
}

Completion Engine::inspect(bytes_view payload, OutputSink &sink) {
    try {
        return m->handle_inspect(payload, sink);
    } catch (const std::exception &e) {
        return m->fail(sink, std::string("internal error: ") + e.what());
    } catch (...) {
        return m->fail(sink, "internal error");
    }
}

Progress Engine::progress() const {
    return Progress{m->meta.executed_input_count(), m->meta.last_executed_safe_block()};
}

uint64_t Engine::rejected_count() const {
    return m->meta.rejected_count();
}

uint32_t Engine::nonce_of(const Address &sender) const {
    return m->nonces.get(sender);
}

Address Engine::sequencer() const {
    return m->meta.sequencer();
}

Address Engine::token() const {
    return m->meta.token();
}

void Engine::set_scheduler_status(uint64_t next_batch_nonce, uint64_t queued_directs) {
    m->scheduler_known = true;
    m->scheduler_next_nonce = next_batch_nonce;
    m->scheduler_queued = queued_directs;
}

const Config &Engine::config() const {
    return m->cfg;
}

Ledger &Engine::ledger() {
    return m->ledger;
}

void Engine::flush() {
    m->ledger.flush();
}

} // namespace qawallet
