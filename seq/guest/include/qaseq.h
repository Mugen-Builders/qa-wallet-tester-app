/* The canonical scheduler fold of the Cartesi sequencer (sequencer-core, v0.1.0-alpha.10) over the
 * qa-wallet engine, as a C API for the machine driver (src/seq_main.cpp). The engine is reached
 * through application-engine.h exactly as on the host (c-app-engine's EngineApp); this layer only
 * adds the Scheduler<EngineApp> fold: direct inputs queue until a batch frame's safe block covers
 * them or they become overdue, batches from the sequencer address execute their frames.
 * Every function returns 0 on success or -1 with the reason in qaseq_last_error(). */
#ifndef QASEQ_H
#define QASEQ_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qaseq qaseq_t;

/* Opens the engine over `dump` (application_engine_from_dump; "state,ledger" names a live machine)
 * and starts the fold expecting batch nonce `next_batch_nonce` (0 at genesis, when the engine's
 * executed input count must be 0 too). */
qaseq_t *qaseq_new(const char *dump, const uint8_t sequencer[20], uint64_t next_batch_nonce);
void qaseq_free(qaseq_t *q);

enum qaseq_outcome_kind {
    QASEQ_DIRECT_ENQUEUED = 0,
    QASEQ_BATCH_EXECUTED = 1,
    QASEQ_BATCH_SKIPPED_STALE = 2,
    QASEQ_BATCH_REJECTED = 3,
};

enum qaseq_reject_reason {
    QASEQ_REJECT_DECODE_FAILED = 0,
    QASEQ_REJECT_WRONG_NONCE = 1,
    QASEQ_REJECT_SAFE_BLOCK_ABOVE_INCLUSION = 2,
    QASEQ_REJECT_NON_MONOTONIC_SAFE_BLOCKS = 3,
};

typedef struct qaseq_outcome {
    uint32_t kind;           /* enum qaseq_outcome_kind */
    uint32_t reject_reason;  /* enum qaseq_reject_reason, QASEQ_BATCH_REJECTED only */
    uint64_t expected_nonce; /* QASEQ_REJECT_WRONG_NONCE */
    uint64_t got_nonce;      /* QASEQ_REJECT_WRONG_NONCE */
} qaseq_outcome_t;

/* Feeds one L1 input (EvmAdvance metadata + payload). Outputs of every execution it triggers are
 * collected in order and stay readable until the next call. A failure is fatal for the fold. */
int qaseq_process_input(qaseq_t *q, const uint8_t sender[20], uint64_t inclusion_block, uint64_t chain_id,
    const uint8_t app_contract[20], const uint8_t *payload, uint64_t payload_size, qaseq_outcome_t *out);

enum qaseq_output_kind { QASEQ_OUTPUT_VOUCHER = 0, QASEQ_OUTPUT_NOTICE = 1 };

typedef struct qaseq_output {
    uint32_t kind;           /* enum qaseq_output_kind */
    uint8_t destination[20]; /* voucher */
    uint8_t value[32];       /* voucher, big-endian */
    const uint8_t *data;     /* notice payload or voucher payload; valid until the next process_input */
    uint64_t size;
} qaseq_output_t;

uint64_t qaseq_output_count(const qaseq_t *q);
int qaseq_output(const qaseq_t *q, uint64_t index, qaseq_output_t *out);

uint64_t qaseq_next_batch_nonce(const qaseq_t *q);
uint64_t qaseq_queued_directs(const qaseq_t *q);
uint64_t qaseq_max_wait_blocks(void);

/* The engine handle behind the fold (for the extensions of seq_ext.h: aux sink, input context,
 * inspects). Owned by the fold; never destroy it. */
struct ApplicationEngine *qaseq_engine(qaseq_t *q);

const char *qaseq_last_error(void);

#ifdef __cplusplus
}
#endif

#endif
