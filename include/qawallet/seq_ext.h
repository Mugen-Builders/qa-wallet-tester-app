/* qa-wallet extensions next to the sequencer's application-engine.h seam. Nothing here is part of
 * the seam: the sequencer host never calls these. They serve the genesis tool, the sequencer guest
 * driver (src/seq_main.cpp), which owns the rollup device and needs the engine's reports, inspects
 * and the per-input metadata the seam does not carry, and the tests.
 * Status codes follow application-engine.h (0 ok, negative failure) and leave the reason in
 * application_engine_get_last_error_message(). */
#ifndef QAWALLET_SEQ_EXT_H
#define QAWALLET_SEQ_EXT_H

#include "application-engine.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Writes a genesis state into `state_path` (a file, created or grown, or the machine's NVRAM).
 * Configuration comes from the QA_* environment like the drivers'; `token` (NULL or all zero: the
 * environment's / devnet default) is the accounts-drive ERC-20, pinned in the state together with
 * the batch submitter. Refuses an initialised state. */
int qawallet_genesis(const char *state_path, const uint8_t sequencer[20], const uint8_t token[20]);

/* The functions below act on the live engine: the one application_engine_from_dump opened over a
 * character device, the machine's NVRAM (there is exactly one in the guest; the host never opens
 * one). They return -ENOENT while no live engine exists. */

/* Where the engine sends what cannot cross the seam: reports. The callback may be NULL (reports
 * are then logged and dropped). It returns 0 or a negative errno; the engine treats a failure like
 * a device error. */
typedef struct qawallet_aux_sink {
    void *ctx;
    int (*report)(void *ctx, const uint8_t *data, uint64_t size);
} qawallet_aux_sink;

int qawallet_live_attach_aux(const qawallet_aux_sink *aux);

/* Input metadata the seam does not carry (EvmAdvance fields). Used for reports and output
 * encoding only (ERC-721/1155 withdrawal vouchers name the app contract); the state never depends
 * on it. Stays in effect until replaced; zero on the host. */
typedef struct qawallet_input_context {
    uint64_t block_timestamp;
    uint64_t index;
    uint64_t chain_id;
    uint8_t app_contract[20];
} qawallet_input_context;

int qawallet_live_set_input_context(const qawallet_input_context *context);

/* Scheduler facts shown by the `state` inspect report. */
int qawallet_live_set_scheduler_status(uint64_t next_batch_nonce, uint64_t queued_directs);

/* Runs an inspect query; reports go to `report` in order. A rejected inspect (unknown method,
 * ledger error) has already written its error report when this returns 0. */
int qawallet_live_inspect(const uint8_t *query, uint64_t size,
    int (*report)(void *ctx, const uint8_t *data, uint64_t size), void *ctx);

/* Test hook: makes any engine (a host dump) the live one, so the extensions can be exercised
 * without a machine. NULL clears it. */
void qawallet_set_live_engine(ApplicationEngine *engine);

/* The fee port (include/qawallet/fee.h): 0 with the big-endian amount in `out`, -1 when the
 * exponent exceeds qawallet_fee_max_exponent(). */
int qawallet_fee_to_linear(uint16_t exponent, uint8_t out[32]);
uint16_t qawallet_fee_max_exponent(void);

#ifdef __cplusplus
}
#endif

#endif
