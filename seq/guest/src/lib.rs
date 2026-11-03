//! The canonical scheduler fold of the Cartesi sequencer over the qa-wallet engine, as a C API for
//! the machine driver (include/qaseq.h). Everything that decides ordering is
//! `sequencer_core::scheduler::Scheduler`, the same code the sequencer's own canonical app runs;
//! the engine is reached through `c_app_engine::EngineApp`, the same seam the host sequencer uses.

use std::cell::RefCell;
use std::ffi::{CStr, CString, c_char};
use std::path::Path;

use alloy_primitives::Address;
use c_app_engine::{Application, EngineApp};
use sequencer_core::application::AppOutput;
use sequencer_core::scheduler::{
    BatchRejectReason, MAX_WAIT_BLOCKS, ProcessOutcome, Scheduler, SchedulerConfig, SchedulerInput,
    input_domain,
};

pub const QASEQ_DIRECT_ENQUEUED: u32 = 0;
pub const QASEQ_BATCH_EXECUTED: u32 = 1;
pub const QASEQ_BATCH_SKIPPED_STALE: u32 = 2;
pub const QASEQ_BATCH_REJECTED: u32 = 3;

pub const QASEQ_REJECT_DECODE_FAILED: u32 = 0;
pub const QASEQ_REJECT_WRONG_NONCE: u32 = 1;
pub const QASEQ_REJECT_SAFE_BLOCK_ABOVE_INCLUSION: u32 = 2;
pub const QASEQ_REJECT_NON_MONOTONIC_SAFE_BLOCKS: u32 = 3;

pub const QASEQ_OUTPUT_VOUCHER: u32 = 0;
pub const QASEQ_OUTPUT_NOTICE: u32 = 1;

/// The fold plus the outputs of the last processed input.
#[allow(non_camel_case_types)]
pub struct qaseq {
    scheduler: Scheduler<EngineApp>,
    outputs: Vec<AppOutput>,
}

#[repr(C)]
#[derive(Debug, Default, Clone, Copy)]
pub struct qaseq_outcome_t {
    pub kind: u32,
    pub reject_reason: u32,
    pub expected_nonce: u64,
    pub got_nonce: u64,
}

#[repr(C)]
#[derive(Debug, Clone, Copy)]
pub struct qaseq_output_t {
    pub kind: u32,
    pub destination: [u8; 20],
    pub value: [u8; 32],
    pub data: *const u8,
    pub size: u64,
}

thread_local! {
    static LAST_ERROR: RefCell<CString> = RefCell::new(CString::default());
}

fn set_error(message: impl Into<String>) {
    let message: String = message.into();
    let message = CString::new(message.replace('\0', "?")).unwrap_or_default();
    LAST_ERROR.with(|e| *e.borrow_mut() = message);
}

fn address(bytes: *const u8) -> Address {
    // SAFETY: the C API promises 20 readable bytes behind every address pointer.
    Address::from_slice(unsafe { std::slice::from_raw_parts(bytes, 20) })
}

/// Opens the engine over `dump` and starts the fold at `next_batch_nonce`.
///
/// # Safety
/// `dump` is a NUL-terminated string and `sequencer` points at 20 bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qaseq_new(
    dump: *const c_char,
    sequencer: *const u8,
    next_batch_nonce: u64,
) -> *mut qaseq {
    if dump.is_null() || sequencer.is_null() {
        set_error("qaseq_new: null argument");
        return std::ptr::null_mut();
    }
    // SAFETY: the caller promises a NUL-terminated string.
    let dump = match unsafe { CStr::from_ptr(dump) }.to_str() {
        Ok(s) => s.to_owned(),
        Err(_) => {
            set_error("qaseq_new: dump path is not UTF-8");
            return std::ptr::null_mut();
        }
    };
    let app = match EngineApp::from_dump(Path::new(&dump)) {
        Ok(app) => app,
        Err(err) => {
            set_error(format!("qaseq_new: {err}"));
            return std::ptr::null_mut();
        }
    };
    let config = SchedulerConfig::new(address(sequencer));
    let count = app.executed_input_count();
    let scheduler = if next_batch_nonce == 0 && count == sequencer_core::history::ExecutedInputCount::ZERO
    {
        Scheduler::new(app, config)
    } else {
        Scheduler::resume_at(app, config, next_batch_nonce)
    };
    Box::into_raw(Box::new(qaseq {
        scheduler,
        outputs: Vec::new(),
    }))
}

/// # Safety
/// `q` came from `qaseq_new` and is not used afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qaseq_free(q: *mut qaseq) {
    if !q.is_null() {
        // SAFETY: ownership returns to Rust exactly once.
        drop(unsafe { Box::from_raw(q) });
    }
}

/// # Safety
/// Pointers follow include/qaseq.h: 20-byte addresses, `payload_size` readable payload bytes,
/// a writable `out`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qaseq_process_input(
    q: *mut qaseq,
    sender: *const u8,
    inclusion_block: u64,
    chain_id: u64,
    app_contract: *const u8,
    payload: *const u8,
    payload_size: u64,
    out: *mut qaseq_outcome_t,
) -> i32 {
    if q.is_null() || sender.is_null() || app_contract.is_null() || out.is_null() {
        set_error("qaseq_process_input: null argument");
        return -1;
    }
    let Ok(len) = usize::try_from(payload_size) else {
        set_error("qaseq_process_input: payload too large");
        return -1;
    };
    let payload = if len == 0 {
        Vec::new()
    } else {
        // SAFETY: the caller promises `payload_size` readable bytes.
        unsafe { std::slice::from_raw_parts(payload, len) }.to_vec()
    };
    // SAFETY: `q` is a live fold handle.
    let fold = unsafe { &mut *q };
    fold.outputs.clear();
    let input = SchedulerInput {
        sender: address(sender),
        inclusion_block,
        domain: input_domain(chain_id, address(app_contract)),
        payload,
    };
    let result = match fold.scheduler.process_input(input) {
        Ok(result) => result,
        Err(err) => {
            set_error(format!("qaseq_process_input: application execution failed: {err}"));
            return -1;
        }
    };
    let mut outcome = qaseq_outcome_t::default();
    match result.outcome {
        ProcessOutcome::DirectEnqueued => outcome.kind = QASEQ_DIRECT_ENQUEUED,
        ProcessOutcome::BatchExecuted => outcome.kind = QASEQ_BATCH_EXECUTED,
        ProcessOutcome::BatchSkippedStale => outcome.kind = QASEQ_BATCH_SKIPPED_STALE,
        ProcessOutcome::BatchRejected(reason) => {
            outcome.kind = QASEQ_BATCH_REJECTED;
            outcome.reject_reason = match reason {
                BatchRejectReason::DecodeFailed => QASEQ_REJECT_DECODE_FAILED,
                BatchRejectReason::WrongNonce { expected, got } => {
                    outcome.expected_nonce = expected;
                    outcome.got_nonce = got;
                    QASEQ_REJECT_WRONG_NONCE
                }
                BatchRejectReason::SafeBlockAboveInclusionBlock => {
                    QASEQ_REJECT_SAFE_BLOCK_ABOVE_INCLUSION
                }
                BatchRejectReason::NonMonotonicSafeBlocks => QASEQ_REJECT_NON_MONOTONIC_SAFE_BLOCKS,
            };
        }
    }
    fold.outputs = result.outputs;
    // SAFETY: `out` is writable.
    unsafe { *out = outcome };
    0
}

/// # Safety
/// `q` is a live fold handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qaseq_output_count(q: *const qaseq) -> u64 {
    if q.is_null() {
        return 0;
    }
    // SAFETY: `q` is a live fold handle.
    unsafe { &*q }.outputs.len() as u64
}

/// # Safety
/// `q` is a live fold handle and `out` is writable. The data pointer stays valid until the next
/// `qaseq_process_input`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qaseq_output(q: *const qaseq, index: u64, out: *mut qaseq_output_t) -> i32 {
    if q.is_null() || out.is_null() {
        set_error("qaseq_output: null argument");
        return -1;
    }
    // SAFETY: `q` is a live fold handle.
    let fold = unsafe { &*q };
    let Some(output) = usize::try_from(index).ok().and_then(|i| fold.outputs.get(i)) else {
        set_error(format!("qaseq_output: no output at index {index}"));
        return -1;
    };
    let view = match output {
        AppOutput::Notice(payload) => qaseq_output_t {
            kind: QASEQ_OUTPUT_NOTICE,
            destination: [0; 20],
            value: [0; 32],
            data: payload.as_ptr(),
            size: payload.len() as u64,
        },
        AppOutput::Voucher {
            destination,
            value,
            payload,
        } => qaseq_output_t {
            kind: QASEQ_OUTPUT_VOUCHER,
            destination: destination.into_array(),
            value: value.to_be_bytes(),
            data: payload.as_ptr(),
            size: payload.len() as u64,
        },
    };
    // SAFETY: `out` is writable.
    unsafe { *out = view };
    0
}

/// # Safety
/// `q` is a live fold handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qaseq_next_batch_nonce(q: *const qaseq) -> u64 {
    if q.is_null() {
        return 0;
    }
    // SAFETY: `q` is a live fold handle.
    unsafe { &*q }.scheduler.next_expected_batch_nonce()
}

/// # Safety
/// `q` is a live fold handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn qaseq_queued_directs(q: *const qaseq) -> u64 {
    if q.is_null() {
        return 0;
    }
    // SAFETY: `q` is a live fold handle.
    unsafe { &*q }.scheduler.queued_direct_len() as u64
}

#[unsafe(no_mangle)]
pub extern "C" fn qaseq_max_wait_blocks() -> u64 {
    MAX_WAIT_BLOCKS
}

#[unsafe(no_mangle)]
pub extern "C" fn qaseq_last_error() -> *const c_char {
    LAST_ERROR.with(|e| e.borrow().as_ptr())
}
