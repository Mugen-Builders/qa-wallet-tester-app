//! The fold over the C seam, against the real engine archive (QAWALLET_LIB_DIR, `make seq-test`):
//! the fee port agrees with sequencer-core for every exponent, direct inputs wait for a covering
//! frame, batches are validated, signed user ops execute and consume nonces and fees.

use std::ffi::CString;
use std::path::PathBuf;
use std::sync::Mutex;

use alloy_primitives::{Address, Signature, U256, address};
use alloy_sol_types::SolStruct;
use k256::ecdsa::SigningKey;
use k256::ecdsa::signature::hazmat::PrehashSigner;
use qawallet_seq_guest::*;
use sequencer_core::batch::{Batch, Frame, WireUserOp};
use sequencer_core::fee::{MAX_EXPONENT, fee_to_linear};
use sequencer_core::scheduler::input_domain;
use sequencer_core::user_op::UserOp;
use ssz::Encode;

unsafe extern "C" {
    fn qawallet_genesis(state: *const std::ffi::c_char, sequencer: *const u8, token: *const u8) -> i32;
    fn qawallet_fee_to_linear(exponent: u16, out: *mut u8) -> i32;
    fn qawallet_fee_max_exponent() -> u16;
    fn application_engine_get_last_error_message() -> *const std::ffi::c_char;
}

const SEQUENCER: Address = address!("0xa0Ee7A142d267C1f36714E4a8F75612F20a79720");
const ERC20_PORTAL: Address = address!("0x3332DE61a8BB9aC84893b2f552Fe81C9a6dC5419");
const TOKEN: Address = address!("0x7a051EDffC0884cd88d4a377F4C87BE074CF6c81");
const APP: Address = address!("0xabababababababababababababababababababab");
const CHAIN_ID: u64 = 31337;

// The engine reads its configuration from the environment: one small layout for every test.
static ENV: Mutex<()> = Mutex::new(());

fn configure(workdir: &std::path::Path) {
    let _guard = ENV.lock().unwrap_or_else(|e| e.into_inner());
    for (k, v) in [
        ("QA_LOG", "0".to_string()),
        ("QA_SINGLE_ACCOUNTS", "1024".to_string()),
        ("QA_REGION_A_LENGTH", (2u64 << 20).to_string()),
        ("QA_REGION_B_LENGTH", (2u64 << 20).to_string()),
        ("QA_REGION_C_LENGTH", (64u64 << 10).to_string()),
        ("QA_MULTI_ACCOUNTS", "256".to_string()),
        ("QA_MULTI_ASSETS", "16".to_string()),
        ("QA_MULTI_BALANCES", "1024".to_string()),
        ("QA_ENGINE_WORKDIR", workdir.display().to_string()),
    ] {
        // SAFETY: tests set the same values; the engine reads them at open time only.
        unsafe { std::env::set_var(k, v) };
    }
}

fn temp_dir(name: &str) -> PathBuf {
    let dir = std::env::temp_dir().join(format!("qawallet-seq-{name}-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&dir);
    std::fs::create_dir_all(&dir).expect("temp dir");
    dir
}

fn genesis(dir: &std::path::Path) -> CString {
    let state = CString::new(dir.join("genesis").to_str().unwrap()).unwrap();
    let rc = unsafe { qawallet_genesis(state.as_ptr(), SEQUENCER.as_ptr(), std::ptr::null()) };
    assert_eq!(rc, 0, "genesis: {}", last_error());
    state
}

fn last_error() -> String {
    unsafe { std::ffi::CStr::from_ptr(application_engine_get_last_error_message()) }
        .to_string_lossy()
        .into_owned()
}

fn fold_error() -> String {
    unsafe { std::ffi::CStr::from_ptr(qaseq_last_error()) }
        .to_string_lossy()
        .into_owned()
}

struct Fold(*mut qaseq);

impl Fold {
    fn new(state: &CString) -> Self {
        let q = unsafe { qaseq_new(state.as_ptr(), SEQUENCER.as_ptr(), 0) };
        assert!(!q.is_null(), "qaseq_new: {}", fold_error());
        Self(q)
    }

    fn process(&mut self, sender: Address, block: u64, payload: &[u8]) -> qaseq_outcome_t {
        let mut out = qaseq_outcome_t::default();
        let rc = unsafe {
            qaseq_process_input(
                self.0,
                sender.as_ptr(),
                block,
                CHAIN_ID,
                APP.as_ptr(),
                payload.as_ptr(),
                payload.len() as u64,
                &mut out,
            )
        };
        assert_eq!(rc, 0, "process_input: {}", fold_error());
        out
    }

    fn outputs(&self) -> Vec<(u32, Vec<u8>, Address, U256)> {
        let n = unsafe { qaseq_output_count(self.0) };
        (0..n)
            .map(|i| {
                let mut o = qaseq_output_t {
                    kind: 0,
                    destination: [0; 20],
                    value: [0; 32],
                    data: std::ptr::null(),
                    size: 0,
                };
                assert_eq!(unsafe { qaseq_output(self.0, i, &mut o) }, 0);
                let data = unsafe { std::slice::from_raw_parts(o.data, o.size as usize) }.to_vec();
                (o.kind, data, Address::from(o.destination), U256::from_be_bytes(o.value))
            })
            .collect()
    }

    fn next_nonce(&self) -> u64 {
        unsafe { qaseq_next_batch_nonce(self.0) }
    }

    fn queued(&self) -> u64 {
        unsafe { qaseq_queued_directs(self.0) }
    }
}

impl Drop for Fold {
    fn drop(&mut self) {
        unsafe { qaseq_free(self.0) };
    }
}

fn word(v: u64) -> [u8; 32] {
    U256::from(v).to_be_bytes()
}

/// Anything that is not a libcma ledger input is echoed back as one notice with the same bytes.
fn echo(data: &[u8]) -> Vec<u8> {
    data.to_vec()
}

/// WithdrawErc20(address,uint256,bytes) of `amount` units of the accounts-drive token.
fn withdraw_erc20(amount: U256) -> Vec<u8> {
    let mut p = vec![0x4f, 0x94, 0xd3, 0x42];
    p.extend_from_slice(&U256::from_be_slice(TOKEN.as_slice()).to_be_bytes::<32>());
    p.extend_from_slice(&amount.to_be_bytes::<32>());
    p.extend_from_slice(&word(96));
    p.extend_from_slice(&word(0));
    p
}

fn erc20_deposit(sender: Address, amount: u64) -> Vec<u8> {
    let mut p = Vec::new();
    p.extend_from_slice(TOKEN.as_slice());
    p.extend_from_slice(sender.as_slice());
    p.extend_from_slice(&word(amount));
    p
}

fn batch(nonce: u64, frames: Vec<Frame>) -> Vec<u8> {
    Batch { nonce, frames }.as_ssz_bytes()
}

fn frame(safe_block: u64, fee_price: u16, user_ops: Vec<WireUserOp>) -> Frame {
    Frame {
        user_ops,
        safe_block,
        fee_price,
    }
}

fn signer(seed: u8) -> (SigningKey, Address) {
    let key = SigningKey::from_bytes((&[seed; 32]).into()).expect("key");
    let verifying = key.verifying_key().to_encoded_point(false);
    (key.clone(), Address::from_raw_public_key(&verifying.as_bytes()[1..]))
}

fn sign(key: &SigningKey, sender: Address, nonce: u32, max_fee: u16, data: Vec<u8>) -> WireUserOp {
    let domain = input_domain(CHAIN_ID, APP);
    let op = UserOp {
        nonce,
        max_fee,
        data: data.clone().into(),
    };
    let hash = op.eip712_signing_hash(&domain);
    let sig = key.sign_prehash(hash.as_slice()).expect("sign");
    let signature = [false, true]
        .into_iter()
        .map(|parity| Signature::from_signature_and_parity(sig, parity))
        .find(|s| s.recover_address_from_prehash(&hash).ok() == Some(sender))
        .expect("recoverable parity");
    WireUserOp {
        nonce,
        max_fee,
        data,
        signature: signature.as_bytes().to_vec(),
    }
}

#[test]
fn fee_port_matches_sequencer_core_for_every_exponent() {
    assert_eq!(unsafe { qawallet_fee_max_exponent() }, MAX_EXPONENT);
    for e in 0..=MAX_EXPONENT {
        let mut out = [0u8; 32];
        assert_eq!(unsafe { qawallet_fee_to_linear(e, out.as_mut_ptr()) }, 0, "exponent {e}");
        assert_eq!(U256::from_be_bytes(out), fee_to_linear(e), "exponent {e}");
    }
    let mut out = [0u8; 32];
    assert_eq!(unsafe { qawallet_fee_to_linear(MAX_EXPONENT + 1, out.as_mut_ptr()) }, -1);
}

#[test]
fn ssz_vector_for_the_python_harness() {
    // tests/machine/harness.py encodes the same batch by hand; keep the two in step
    let bytes = batch(0, vec![frame(10, 0, vec![])]);
    // nonce 0, frames offset 12, frame offset 4, user_ops offset 14, safe block 10, fee price 0
    assert_eq!(
        alloy_primitives::hex::encode(&bytes),
        "00000000000000000c000000040000000e0000000a000000000000000000"
    );
    let (key, sender) = signer(1);
    let op = sign(&key, sender, 0, 7, vec![0xaa, 0xbb]);
    let with_op = batch(3, vec![frame(20, 5, vec![op.clone()])]);
    let hex = alloy_primitives::hex::encode(&with_op);
    // fixed parts: nonce, frames offset; frame offsets; frame fixed part (user_ops offset, safe
    // block, fee price); user op offsets; user op fixed part (nonce, max fee, data offset,
    // signature offset); then data and the 65-byte signature
    assert!(hex.starts_with("03000000000000000c000000040000000e00000014000000000000000500"), "{hex}");
    assert!(hex.ends_with(&alloy_primitives::hex::encode(&op.signature)));
    assert_eq!(with_op.len(), 12 + 4 + 14 + 4 + 14 + 2 + 65);
    println!("ssz batch vector: {hex}");
}

#[test]
fn directs_wait_for_a_covering_frame_and_batches_are_validated() {
    let dir = temp_dir("fold");
    configure(&dir);
    let state = genesis(&dir);
    let mut fold = Fold::new(&state);
    let user = address!("0x1111111111111111111111111111111111111111");

    // a deposit waits in the fridge
    let out = fold.process(ERC20_PORTAL, 10, &erc20_deposit(user, 1_000_000));
    assert_eq!(out.kind, QASEQ_DIRECT_ENQUEUED);
    assert_eq!((fold.queued(), fold.next_nonce()), (1, 0));
    assert!(fold.outputs().is_empty());

    // batches from anyone but the submitter are directs too
    let out = fold.process(user, 11, &batch(0, vec![frame(11, 0, vec![])]));
    assert_eq!(out.kind, QASEQ_DIRECT_ENQUEUED);
    assert_eq!(fold.queued(), 2);

    // wrong nonce, undecodable, safe block above inclusion: rejected without consuming the nonce
    let out = fold.process(SEQUENCER, 12, &batch(1, vec![frame(12, 0, vec![])]));
    assert_eq!((out.kind, out.reject_reason), (QASEQ_BATCH_REJECTED, QASEQ_REJECT_WRONG_NONCE));
    assert_eq!((out.expected_nonce, out.got_nonce), (0, 1));
    let out = fold.process(SEQUENCER, 12, &[0xff, 0xee]);
    assert_eq!((out.kind, out.reject_reason), (QASEQ_BATCH_REJECTED, QASEQ_REJECT_DECODE_FAILED));
    let out = fold.process(SEQUENCER, 12, &batch(0, vec![frame(13, 0, vec![])]));
    assert_eq!(out.reject_reason, QASEQ_REJECT_SAFE_BLOCK_ABOVE_INCLUSION);
    assert_eq!((fold.queued(), fold.next_nonce()), (2, 0));

    // a frame covering block 10 drains the deposit only
    let out = fold.process(SEQUENCER, 15, &batch(0, vec![frame(10, 0, vec![])]));
    assert_eq!(out.kind, QASEQ_BATCH_EXECUTED);
    assert_eq!((fold.queued(), fold.next_nonce()), (1, 1));

    // echoes are returned in order when their directs are drained
    let out = fold.process(user, 20, &echo(&[0xff; 8]));
    assert_eq!(out.kind, QASEQ_DIRECT_ENQUEUED);
    let out = fold.process(user, 20, &echo(&[0xde, 0xad, 0xbe, 0xef]));
    assert_eq!(out.kind, QASEQ_DIRECT_ENQUEUED);
    let out = fold.process(SEQUENCER, 25, &batch(1, vec![frame(20, 0, vec![])]));
    assert_eq!(out.kind, QASEQ_BATCH_EXECUTED);
    let outputs = fold.outputs();
    // the user's "batch" of block 11 is not a ledger input either: it is echoed first
    assert_eq!(outputs.len(), 3);
    assert!(outputs.iter().all(|(kind, _, _, _)| *kind == QASEQ_OUTPUT_NOTICE));
    assert_eq!(outputs[0].1, batch(0, vec![frame(11, 0, vec![])]));
    assert_eq!(outputs[1].1, vec![0xff; 8]);
    assert_eq!(outputs[2].1, vec![0xde, 0xad, 0xbe, 0xef]);
    assert_eq!((fold.queued(), fold.next_nonce()), (0, 2));

    // a stale batch is skipped and keeps the nonce; its direct is then force-drained by the next input
    let out = fold.process(user, 30, &echo(&[1]));
    assert_eq!(out.kind, QASEQ_DIRECT_ENQUEUED);
    let stale = 30 + qaseq_max_wait_blocks();
    let out = fold.process(SEQUENCER, stale, &batch(2, vec![frame(30, 0, vec![])]));
    assert_eq!(out.kind, QASEQ_BATCH_SKIPPED_STALE);
    assert_eq!(fold.next_nonce(), 2);
    assert_eq!(fold.outputs().len(), 1, "the overdue direct ran before the stale batch was judged");
    assert_eq!(fold.queued(), 0);
    let _ = std::fs::remove_dir_all(&dir);
}

#[test]
fn signed_user_ops_execute_consume_nonces_and_pay_fees() {
    let dir = temp_dir("userops");
    configure(&dir);
    let state = genesis(&dir);
    let mut fold = Fold::new(&state);
    let (key, sender) = signer(7);
    let required = fee_to_linear(100);

    // the sender must hold the fee token first (drained at the first frame)
    fold.process(ERC20_PORTAL, 10, &erc20_deposit(sender, 1_000_000));
    let op0 = sign(&key, sender, 0, 100, echo(&[0x42]));
    let out = fold.process(SEQUENCER, 15, &batch(0, vec![frame(10, 100, vec![op0.clone()])]));
    assert_eq!(out.kind, QASEQ_BATCH_EXECUTED);
    let outputs = fold.outputs();
    assert_eq!(outputs.len(), 1);
    assert_eq!(outputs[0].1, vec![0x42]);

    // replaying nonce 0 is refused silently; nonce 1 runs; a max_fee below the frame fee is skipped
    let op0_again = op0.clone();
    let op1 = sign(&key, sender, 1, 100, echo(&[0x43]));
    let cheap = sign(&key, sender, 2, 1, echo(&[0x44]));
    let out = fold.process(SEQUENCER, 20, &batch(1, vec![frame(20, 100, vec![op0_again, op1, cheap])]));
    assert_eq!(out.kind, QASEQ_BATCH_EXECUTED);
    let outputs = fold.outputs();
    assert_eq!(outputs.len(), 1);
    assert_eq!(outputs[0].1, vec![0x43]);

    // the fees reached the submitter: it can now sign a user op itself (fee 0 -> 1 unit)
    // (two ops of fee 100 were charged: 2 * required units)
    assert!(required > U256::ZERO);
    // WithdrawErc20(address,uint256,bytes): the submitter withdraws its fee income
    let withdraw = withdraw_erc20(required * U256::from(2));
    let out = fold.process(SEQUENCER, 25, &withdraw);
    assert_eq!(out.kind, QASEQ_BATCH_REJECTED, "the submitter's inputs are batches, never directs");
    let (skey, _) = signer(9);
    let _ = skey;
    let out = fold.process(user_as_direct(sender), 26, &withdraw);
    assert_eq!(out.kind, QASEQ_DIRECT_ENQUEUED);
    let out = fold.process(SEQUENCER, 30, &batch(2, vec![frame(26, 0, vec![])]));
    assert_eq!(out.kind, QASEQ_BATCH_EXECUTED);
    let outputs = fold.outputs();
    // the user's own balance covers 2*required, so the direct withdrawal emits a voucher
    assert_eq!(outputs.len(), 1);
    assert_eq!(outputs[0].0, QASEQ_OUTPUT_VOUCHER);
    assert_eq!(outputs[0].2, TOKEN);

    // a business failure (overdraft) as a user op still consumes the nonce and the fee: no output
    let over = sign(&key, sender, 2, 100, withdraw_erc20(U256::from(1_000_000_000u64)));
    let out = fold.process(SEQUENCER, 35, &batch(3, vec![frame(35, 100, vec![over])]));
    assert_eq!(out.kind, QASEQ_BATCH_EXECUTED);
    assert!(fold.outputs().is_empty());
    let replay = sign(&key, sender, 2, 100, echo(&[0x45]));
    let out = fold.process(SEQUENCER, 40, &batch(4, vec![frame(40, 100, vec![replay])]));
    assert_eq!(out.kind, QASEQ_BATCH_EXECUTED);
    assert!(fold.outputs().is_empty(), "nonce 2 was consumed by the failed op");
    let _ = std::fs::remove_dir_all(&dir);
}

fn user_as_direct(sender: Address) -> Address {
    sender
}
