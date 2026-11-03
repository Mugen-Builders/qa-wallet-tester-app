#include "qawallet/config.h"

#include <cstdlib>
#include <sstream>
#include <string>

namespace qawallet {

static Address must_addr(const char *hex) {
    Address a;
    if (!parse_address(hex, a)) {
        std::abort(); // compile-time constants below are well-formed
    }
    return a;
}

Config Config::devnet_defaults() {
    Config c;
    // rollups-contracts v3.0.0-alpha.10 devnet (`cartesi address-book`, CLI 2.0.0-alpha.38)
    c.token = must_addr("0x7a051EDffC0884cd88d4a377F4C87BE074CF6c81"); // TestUsdc
    c.ether_portal = must_addr("0x035b11Be55656c6cfC822D1CaE568C1Af2e497b0");
    c.erc20_portal = must_addr("0x3332DE61a8BB9aC84893b2f552Fe81C9a6dC5419");
    c.erc721_portal = must_addr("0x397c352d18DFf47CC8a6143403142cf7afd5Ff7E");
    c.erc1155_single_portal = must_addr("0x585F56351A66f131E176a345662215C772f80451");
    c.erc1155_batch_portal = must_addr("0xee33550a22e3Cf6Cc265524dC9bcfD99D2307EBe");
    return c;
}

static bool env_addr(const char *name, Address &out, std::string &err) {
    const char *v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return true;
    }
    if (!parse_address(v, out)) {
        err = std::string(name) + ": invalid address '" + v + "'";
        return false;
    }
    return true;
}

static bool env_u64(const char *name, size_t &out, std::string &err) {
    const char *v = std::getenv(name);
    if (v == nullptr || *v == '\0') {
        return true;
    }
    char *end = nullptr;
    unsigned long long parsed = std::strtoull(v, &end, 0);
    if (end == v || *end != '\0') {
        err = std::string(name) + ": invalid number '" + v + "'";
        return false;
    }
    out = static_cast<size_t>(parsed);
    return true;
}

bool Config::apply_env(std::string &err) {
    if (!env_addr("QA_TOKEN", token, err) || !env_addr("QA_ETHER_PORTAL", ether_portal, err) ||
        !env_addr("QA_ERC20_PORTAL", erc20_portal, err) || !env_addr("QA_ERC721_PORTAL", erc721_portal, err) ||
        !env_addr("QA_ERC1155_SINGLE_PORTAL", erc1155_single_portal, err) ||
        !env_addr("QA_ERC1155_BATCH_PORTAL", erc1155_batch_portal, err)) {
        return false;
    }
    size_t log_v = log ? 1 : 0;
    if (!env_u64("QA_LOG", log_v, err)) {
        return false;
    }
    log = log_v != 0;
    if (!env_u64("QA_SINGLE_ACCOUNTS", single_accounts, err) || !env_u64("QA_REGION_A_LENGTH", region_a_length, err) ||
        !env_u64("QA_REGION_B_LENGTH", region_b_length, err) || !env_u64("QA_REGION_C_LENGTH", region_c_length, err) ||
        !env_u64("QA_MULTI_ACCOUNTS", multi_accounts, err) ||
        !env_u64("QA_MULTI_ASSETS", multi_assets, err) || !env_u64("QA_MULTI_BALANCES", multi_balances, err)) {
        return false;
    }
    if (single_accounts == 0 || (single_accounts & (single_accounts - 1)) != 0) {
        err = "QA_SINGLE_ACCOUNTS: must be a power of two";
        return false;
    }
    if (region_c_length < 2 * 4096 || region_a_length % 4096 != 0 || region_b_length % 4096 != 0 ||
        region_c_length % 4096 != 0) {
        err = "QA_REGION_*_LENGTH: regions must be page multiples and region C at least two pages";
        return false;
    }
    return true;
}

std::string Config::describe_json() const {
    std::ostringstream o;
    o << "{\"token\":\"" << to_hex(token) << "\",\"ether_portal\":\"" << to_hex(ether_portal)
      << "\",\"erc20_portal\":\"" << to_hex(erc20_portal) << "\",\"erc721_portal\":\"" << to_hex(erc721_portal)
      << "\",\"erc1155_single_portal\":\"" << to_hex(erc1155_single_portal) << "\",\"erc1155_batch_portal\":\""
      << to_hex(erc1155_batch_portal) << "\",\"single_accounts\":" << single_accounts
      << ",\"accounts_drive_length\":" << accounts_drive_length() << ",\"region_a_length\":" << region_a_length
      << ",\"region_b_length\":" << region_b_length << ",\"region_c_length\":" << region_c_length << "}";
    return o.str();
}

} // namespace qawallet
