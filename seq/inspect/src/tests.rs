//! Against the real engine archive (APPLICATION_ENGINE_LIB, `make seq-inspect-test`): feed
//! messages reach the engine as the sequencer's replay applies them, inspects answer from that
//! state without changing it, snapshot archives are unpacked defensively, and the whole follower
//! restores, follows and recovers against a stub sequencer.

use std::ffi::CString;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicU64, Ordering};
use std::sync::{Arc, Mutex, Once};
use std::time::Duration;

use alloy_primitives::{Address, address, hex};
use axum::Router;
use axum::extract::ws::{Message, WebSocketUpgrade};
use axum::extract::{Query, State};
use axum::http::{HeaderMap, StatusCode};
use axum::response::{IntoResponse, Response};
use axum::routing::get;
use sequencer_core::api::WsTxMessage;
use sequencer_core::application::Application;
use serde_json::{Value, json};

use crate::engine::Engine;
use crate::follow::{self, Config, Shared, apply, unpack_snapshot};
use crate::http;

unsafe extern "C" {
    fn qawallet_genesis(state: *const std::ffi::c_char, sequencer: *const u8, token: *const u8) -> i32;
}

const SEQUENCER: Address = address!("0xa0Ee7A142d267C1f36714E4a8F75612F20a79720");
const APP: Address = address!("0xabababababababababababababababababababab");
const ALICE: &str = "0x1111111111111111111111111111111111111111";
const ERA: &str = "00000000-0000-4000-8000-000000000001";

/// One small layout for every test; the engine reads it from the environment at open time.
fn temp_dir(name: &str) -> PathBuf {
    static ENV: Once = Once::new();
    let root = std::env::temp_dir().join(format!("qawallet-inspect-test-{}", std::process::id()));
    ENV.call_once(|| {
        let _ = std::fs::remove_dir_all(&root);
        std::fs::create_dir_all(&root).expect("temp root");
        for (k, v) in [
            ("QA_LOG", "0".to_string()),
            ("QA_SINGLE_ACCOUNTS", "1024".to_string()),
            ("QA_REGION_A_LENGTH", (2u64 << 20).to_string()),
            ("QA_REGION_B_LENGTH", (2u64 << 20).to_string()),
            ("QA_REGION_C_LENGTH", (64u64 << 10).to_string()),
            ("QA_MULTI_ACCOUNTS", "256".to_string()),
            ("QA_MULTI_ASSETS", "16".to_string()),
            ("QA_MULTI_BALANCES", "1024".to_string()),
            ("QA_ENGINE_WORKDIR", root.display().to_string()),
        ] {
            // SAFETY: set once, before any engine is opened or any other thread reads them.
            unsafe { std::env::set_var(k, v) };
        }
    });
    let dir = root.join(name);
    std::fs::create_dir_all(&dir).expect("temp dir");
    dir
}

fn genesis(dir: &Path) -> PathBuf {
    let path = dir.join("genesis");
    let c = CString::new(path.to_str().unwrap()).unwrap();
    let rc = unsafe { qawallet_genesis(c.as_ptr(), SEQUENCER.as_ptr(), std::ptr::null()) };
    assert_eq!(rc, 0, "genesis failed");
    path
}

fn direct(offset: u64, block: u64, payload: &[u8]) -> String {
    json!({
        "kind": "direct_input", "offset": offset, "sender": ALICE, "block_number": block,
        "payload": hex::encode_prefixed(payload), "input_index": offset, "batch_nonce": 0,
        "block_timestamp": 0, "transaction_hash": format!("0x{}", "00".repeat(32)),
    })
    .to_string()
}

fn message(text: &str) -> WsTxMessage {
    serde_json::from_str(text).expect("feed message")
}

fn state_of(engine: &mut Engine) -> Value {
    let reports = engine.inspect(b"state").expect("inspect");
    assert_eq!(reports.len(), 1);
    serde_json::from_slice(&reports[0]).expect("state json")
}

#[test]
fn feed_messages_execute_in_order_and_inspects_do_not_change_the_state() {
    let dir = temp_dir("apply");
    let mut engine = Engine::from_dump(&genesis(&dir)).expect("open genesis");
    assert_eq!(engine.executed_input_count().get(), 0);

    apply(&mut engine, message(&direct(0, 5, b"\xde\xad\xbe\xef\x01"))).expect("first input");
    apply(&mut engine, message(&direct(1, 6, b"hello"))).expect("second input");
    assert_eq!(engine.executed_input_count().get(), 2);
    assert_eq!(engine.last_executed_safe_block(), 6);

    // a repeated or skipped offset is a gap: nothing executes
    for offset in [1, 3] {
        let err = apply(&mut engine, message(&direct(offset, 7, b"x"))).unwrap_err();
        assert!(err.contains("feed gap"), "{err}");
    }
    // malformed fields are refused before the engine sees them
    let bad = direct(2, 7, b"x").replace(ALICE, "0x1234");
    assert!(apply(&mut engine, message(&bad)).unwrap_err().contains("sender"));
    assert_eq!(engine.executed_input_count().get(), 2);

    // a user op the engine would not have accepted means the two states differ
    let op = json!({"kind": "user_op", "offset": 2, "sender": ALICE, "nonce": 7, "fee": 0,
                    "data": "0x", "safe_block": 7, "batch_nonce": 0})
    .to_string();
    let err = apply(&mut engine, message(&op)).unwrap_err();
    assert!(err.contains("does not validate"), "{err}");
    assert_eq!(engine.executed_input_count().get(), 2);

    // inspects: the engine's own answers, and no trace in the state
    let state = state_of(&mut engine);
    assert_eq!(state["executed_input_count"], 2);
    assert_eq!(state["state_backend"], "file");
    assert!(state.get("scheduler").is_none());
    let keccak = state["accounts_drive_keccak256"].clone();
    assert_eq!(engine.inspect(b"ping").unwrap(), vec![b"ping".to_vec()]);
    let fee = engine.inspect(br#"{"method":"qa_fee","params":[0]}"#).unwrap();
    assert_eq!(fee[0].len(), 32);
    let unknown = engine.inspect(br#"{"method":"nope","params":[]}"#).unwrap();
    assert!(String::from_utf8_lossy(&unknown[0]).contains("error"));
    for _ in 0..200 {
        engine
            .inspect(format!(r#"{{"method":"ledger_getBalance","params":["{ALICE}"]}}"#).as_bytes())
            .unwrap();
    }
    let after = state_of(&mut engine);
    assert_eq!(after, state);
    assert_eq!(after["accounts_drive_keccak256"], keccak);

    // and the feed continues where it was
    apply(&mut engine, message(&direct(2, 7, b"again"))).expect("third input");
    assert_eq!(engine.executed_input_count().get(), 3);
}

type TarEntry<'a> = (&'a str, tar::EntryType, &'a [u8]);

fn tar_of(entries: &[TarEntry]) -> Vec<u8> {
    let mut builder = tar::Builder::new(Vec::new());
    for (name, kind, data) in entries {
        let mut header = tar::Header::new_gnu();
        header.set_entry_type(*kind);
        header.set_size(data.len() as u64);
        header.set_mode(0o644);
        // set the name bytes directly: the builder refuses to write `..` itself
        let field = &mut header.as_old_mut().name;
        field[..name.len()].copy_from_slice(name.as_bytes());
        header.set_cksum();
        builder.append(&header, *data).unwrap();
    }
    builder.into_inner().unwrap()
}

#[test]
fn snapshot_archives_are_unpacked_defensively() {
    use tar::EntryType::{Directory, Regular, Symlink};
    let dir = temp_dir("unpack");
    let run = |name: &str, entries: &[TarEntry], max: u64| {
        let archive = dir.join(format!("{name}.tar"));
        std::fs::write(&archive, tar_of(entries)).unwrap();
        let out = dir.join(name);
        unpack_snapshot(&archive, &out, max).map(|()| out)
    };

    let out = run("good", &[("info.toml", Regular, b"a = 1\n"), ("./state", Regular, b"STATE")], 64)
        .expect("good archive");
    assert_eq!(std::fs::read(out.join("state")).unwrap(), b"STATE");
    assert_eq!(std::fs::read(out.join("info.toml")).unwrap(), b"a = 1\n");

    let cases: [(&str, &[TarEntry], u64); 7] = [
        ("traversal", &[("info.toml", Regular, b""), ("../state", Regular, b"x")], 64),
        ("absolute", &[("info.toml", Regular, b""), ("/etc/state", Regular, b"x")], 64),
        ("extra", &[("info.toml", Regular, b""), ("state", Regular, b"x"), ("other", Regular, b"x")], 64),
        ("symlink", &[("info.toml", Regular, b""), ("state", Symlink, b"")], 64),
        ("directory", &[("info.toml", Regular, b""), ("state", Directory, b"")], 64),
        ("duplicate", &[("info.toml", Regular, b""), ("state", Regular, b"x"), ("state", Regular, b"y")], 64),
        ("oversize", &[("info.toml", Regular, b"12345"), ("state", Regular, b"123456")], 10),
    ];
    for (name, entries, max) in cases {
        let err = run(name, entries, max).expect_err(name);
        assert_eq!(err.kind(), std::io::ErrorKind::InvalidData, "{name}: {err}");
    }
    let err = run("missing", &[("info.toml", Regular, b"")], 64).expect_err("missing state");
    assert!(err.to_string().contains("missing"), "{err}");
    // a truncated download does not pass for a snapshot
    let truncated = dir.join("truncated.tar");
    let whole = tar_of(&[("info.toml", Regular, b"a"), ("state", Regular, &[7u8; 4096])]);
    std::fs::write(&truncated, &whole[..1500]).unwrap();
    assert!(unpack_snapshot(&truncated, &dir.join("truncated"), 1 << 20).is_err());
    assert!(!dir.join("etc").exists() && !dir.parent().unwrap().join("state").exists());
}

#[test]
fn the_response_has_the_node_shape() {
    let body = http::inspect_response(&[vec![0xab, 0x01], vec![]], 7);
    assert_eq!(
        body,
        json!({"status": "Accepted", "exception_payload": null,
               "reports": [{"payload": "0xab01"}, {"payload": "0x"}], "processed_input_count": 7})
    );
}

// ---------------------------------------------------------------------------- stub sequencer

/// Serves one history at a time: a snapshot (always the genesis), `/history`, and the feed.
struct Stub {
    snapshot: Vec<u8>,
    generation: AtomicU64,
    feed: Mutex<Vec<String>>,
    /// Bumped to hang up on every subscriber, as a restarting sequencer does.
    hangup: tokio::sync::watch::Sender<u64>,
}

#[derive(serde::Deserialize)]
struct Claim {
    era_id: String,
    recovery_generation: u64,
    next_input: u64,
}

async fn stub_snapshot(State(stub): State<Arc<Stub>>) -> Response {
    let mut headers = HeaderMap::new();
    headers.insert("Content-Type", "application/x-tar".parse().unwrap());
    headers.insert("X-History-Era", ERA.parse().unwrap());
    let generation = stub.generation.load(Ordering::SeqCst);
    headers.insert("X-Recovery-Generation", generation.to_string().parse().unwrap());
    headers.insert("X-Executed-Input-Count", "0".parse().unwrap());
    (headers, stub.snapshot.clone()).into_response()
}

async fn stub_history(State(stub): State<Arc<Stub>>) -> Response {
    let head = stub.feed.lock().unwrap().len();
    axum::Json(json!({
        "deployment": {"chain_id": 31337, "app_address": APP, "input_box_address": Address::ZERO,
                       "app_deployment_block": 1, "batch_submitter_address": SEQUENCER},
        "history": {"version": {"era_id": ERA, "recovery_generation": stub.generation.load(Ordering::SeqCst)},
                    "available_from": 0, "head": head},
        "baseline": {"l1_stop_block": 0, "l1_end_input_index": 0, "next_batch_nonce": 0},
        "accepted_checkpoint": null,
        "compatibility": null,
    }))
    .into_response()
}

async fn stub_subscribe(
    State(stub): State<Arc<Stub>>,
    Query(claim): Query<Claim>,
    ws: WebSocketUpgrade,
) -> Response {
    let current = stub.generation.load(Ordering::SeqCst);
    if claim.era_id != ERA || claim.recovery_generation != current {
        let body = json!({"code": "STALE_GENERATION",
                          "current": {"era_id": ERA, "recovery_generation": current}})
        .to_string();
        let mut headers = HeaderMap::new();
        headers.insert("X-History-Error", body.parse().unwrap());
        return (StatusCode::CONFLICT, headers, body).into_response();
    }
    let mut hangup = stub.hangup.subscribe();
    ws.on_upgrade(move |mut socket| async move {
        let mut next = claim.next_input as usize;
        loop {
            let pending: Vec<String> = stub.feed.lock().unwrap().iter().skip(next).cloned().collect();
            for text in pending {
                next += 1;
                if socket.send(Message::Text(text.into())).await.is_err() {
                    return;
                }
            }
            tokio::select! {
                _ = hangup.changed() => return,
                _ = tokio::time::sleep(Duration::from_millis(20)) => {}
            }
        }
    })
}

async fn serve(router: Router) -> String {
    let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
    let url = format!("http://{}", listener.local_addr().unwrap());
    tokio::spawn(async move { axum::serve(listener, router).await.unwrap() });
    url
}

async fn inspect(client: &reqwest::Client, base: &str, app: &str, payload: &[u8]) -> (u16, Value) {
    let response = client
        .post(format!("{base}/inspect/{app}"))
        .body(payload.to_vec())
        .send()
        .await
        .expect("inspect request");
    let status = response.status().as_u16();
    let body = response.bytes().await.unwrap();
    (status, serde_json::from_slice(&body).unwrap_or(Value::Null))
}

async fn wait_for_count(client: &reqwest::Client, base: &str, count: u64) -> Value {
    for _ in 0..600 {
        let (status, body) = inspect(client, base, &APP.to_string(), b"state").await;
        if status == 200 && body["processed_input_count"] == count {
            return body;
        }
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
    panic!("the follower never reached input {count}");
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn the_follower_restores_follows_and_recovers() {
    let dir = temp_dir("follower");
    let state = std::fs::read(genesis(&dir)).unwrap();
    let snapshot = {
        let mut builder = tar::Builder::new(Vec::new());
        for (name, data) in [("info.toml", b"format_version = 2\nnext_batch_nonce = 0\n".as_slice()),
                             ("state", state.as_slice())] {
            let mut header = tar::Header::new_gnu();
            header.set_size(data.len() as u64);
            header.set_mode(0o644);
            builder.append_data(&mut header, name, data).unwrap();
        }
        builder.into_inner().unwrap()
    };
    let stub = Arc::new(Stub {
        snapshot,
        generation: AtomicU64::new(0),
        feed: Mutex::new(vec![direct(0, 5, b"one"), direct(1, 6, b"two")]),
        hangup: tokio::sync::watch::channel(0).0,
    });
    let sequencer = serve(
        Router::new()
            .route("/latest_snapshot", get(stub_snapshot))
            .route("/history", get(stub_history))
            .route("/ws/subscribe", get(stub_subscribe))
            .with_state(stub.clone()),
    )
    .await;

    let shared = Arc::new(Shared::default());
    let client = reqwest::Client::new();
    let base = serve(http::router(shared.clone())).await;
    // nothing is answered before the follower has a state
    assert_eq!(inspect(&client, &base, &APP.to_string(), b"state").await.0, 503);
    assert_eq!(client.get(format!("{base}/readyz")).send().await.unwrap().status(), 503);

    let scratch = dir.join("scratch");
    std::fs::create_dir_all(&scratch).unwrap();
    let follower = tokio::spawn(follow::run(
        shared.clone(),
        Config { sequencer_url: sequencer, work_dir: scratch.clone(), max_snapshot_bytes: 1 << 30 },
    ));

    // snapshot + backlog
    let body = wait_for_count(&client, &base, 2).await;
    assert_eq!(body["status"], "Accepted");
    assert_eq!(body["exception_payload"], Value::Null);
    let report = hex::decode(body["reports"][0]["payload"].as_str().unwrap()).unwrap();
    let state: Value = serde_json::from_slice(&report).unwrap();
    assert_eq!(state["executed_input_count"], 2);
    assert_eq!(client.get(format!("{base}/readyz")).send().await.unwrap().status(), 200);
    assert_eq!(client.get(format!("{base}/healthz")).send().await.unwrap().status(), 200);
    assert_eq!(std::fs::read_dir(&scratch).unwrap().count(), 0, "the download is cleaned up");

    // live suffix
    stub.feed.lock().unwrap().push(direct(2, 7, b"three"));
    wait_for_count(&client, &base, 3).await;

    // the route: raw echo, error reports are still Accepted, other apps and big bodies are refused
    let (status, body) = inspect(&client, &base, &APP.to_string().to_lowercase(), b"ping").await;
    assert_eq!((status, body["reports"][0]["payload"].as_str()), (200, Some("0x70696e67")));
    let (status, body) = inspect(&client, &base, &APP.to_string(), br#"{"method":"nope"}"#).await;
    assert_eq!((status, &body["status"]), (200, &json!("Accepted")));
    let report = hex::decode(body["reports"][0]["payload"].as_str().unwrap()).unwrap();
    assert!(String::from_utf8_lossy(&report).contains("error"));
    assert_eq!(inspect(&client, &base, ALICE, b"state").await.0, 404);
    assert_eq!(inspect(&client, &base, "not-an-address", b"state").await.0, 404);
    let big = vec![b'a'; http::MAX_PAYLOAD_BYTES + 1];
    assert_eq!(inspect(&client, &base, &APP.to_string(), &big).await.0, 413);
    let (status, body) = inspect(&client, &base, &APP.to_string(), &big[1..]).await;
    assert_eq!((status, body["reports"][0]["payload"].as_str().map(str::len)), (200, Some(2 + 2 * http::MAX_PAYLOAD_BYTES)));
    assert_eq!(client.get(format!("{base}/inspect/{APP}")).send().await.unwrap().status(), 405);

    // a dropped connection resumes from the engine's own position, without a new snapshot
    stub.hangup.send_modify(|n| *n += 1);
    stub.feed.lock().unwrap().push(direct(3, 8, b"four"));
    wait_for_count(&client, &base, 4).await;

    // the sequencer recovered: the old claim is refused, the replaced history is adopted
    *stub.feed.lock().unwrap() = vec![direct(0, 9, b"replacement")];
    stub.generation.store(1, Ordering::SeqCst);
    stub.hangup.send_modify(|n| *n += 1);
    let body = wait_for_count(&client, &base, 1).await;
    let report = hex::decode(body["reports"][0]["payload"].as_str().unwrap()).unwrap();
    let state: Value = serde_json::from_slice(&report).unwrap();
    assert_eq!((&state["executed_input_count"], &state["last_block"]), (&json!(1), &json!(9)));

    assert!(!follower.is_finished());
    follower.abort();
}
