// qa-wallet-genesis: writes the host genesis dump of the sequencer variant, the file the sequencer's
// `setup --state-file` registers as its baseline. Its bytes equal the machine's `state` NVRAM after
// the genesis boot when both are given the same batch submitter address (docs/sequencer.md checks
// that with cmp).
//   qa-wallet-genesis <state-file> --sequencer 0x<batch submitter address> [--token 0x<erc20>]
// Configuration comes from the QA_* environment, exactly like the drivers; --token pins the
// accounts-drive ERC-20 like the machine's config range does (default: QA_TOKEN / devnet TestUsdc).
#include "qawallet/seq_ext.h"
#include "qawallet/types.h"

#include <cstdio>
#include <cstring>
#include <string>

using namespace qawallet;

static int usage() {
    std::fprintf(stderr,
        "usage: qa-wallet-genesis <state-file> --sequencer 0x<address> [--token 0x<erc20>]\n"
        "  writes a genesis state (QA_* environment for the configuration); refuses an existing one.\n"
        "  --token pins the accounts-drive ERC-20 (default QA_TOKEN or the devnet TestUsdc).\n");
    return 2;
}

int main(int argc, char *argv[]) {
    std::string state;
    Address sequencer;
    Address token; // zero: the environment's default
    bool have_sequencer = false;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--sequencer" && i + 1 < argc) {
            if (!parse_address(argv[++i], sequencer)) {
                std::fprintf(stderr, "qa-wallet-genesis: invalid address %s\n", argv[i]);
                return 2;
            }
            have_sequencer = true;
        } else if (a == "--token" && i + 1 < argc) {
            if (!parse_address(argv[++i], token)) {
                std::fprintf(stderr, "qa-wallet-genesis: invalid address %s\n", argv[i]);
                return 2;
            }
        } else if (a == "-h" || a == "--help") {
            return usage();
        } else if (state.empty() && !a.empty() && a[0] != '-') {
            state = a;
        } else {
            return usage();
        }
    }
    if (state.empty() || !have_sequencer) {
        return usage();
    }
    int rc = qawallet_genesis(state.c_str(), sequencer.b.data(), token.b.data());
    if (rc != 0) {
        std::fprintf(stderr, "qa-wallet-genesis: %s\n", application_engine_get_last_error_message());
        return 1;
    }
    std::printf("wrote %s (sequencer %s)\n", state.c_str(), to_hex(sequencer).c_str());
    return 0;
}
