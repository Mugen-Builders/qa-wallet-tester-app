//! The follower: restore the sequencer's latest snapshot, then apply its transaction feed.
//!
//! Nothing is persisted. A dropped socket resumes from the engine's own input count; a refused
//! history claim (the sequencer recovered, or its era changed) or any sign of divergence discards
//! the engine and starts again from a fresh snapshot.

use std::fs::{self, File};
use std::io::{self, Read, Write};
use std::os::unix::fs::DirBuilderExt;
use std::path::{Path, PathBuf};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, RwLock};
use std::time::{Duration, Instant};

use alloy_primitives::{Address, hex};
use futures_util::{SinkExt, StreamExt};
use sequencer_core::api::WsTxMessage;
use sequencer_core::application::{
    Application, ValidationOutcome, execute_direct_input, execute_valid_user_op,
};
use sequencer_core::history::{
    EraId, ExecutedInputCount, HistoryClaim, HistoryVersion, RecoveryGeneration,
};
use sequencer_core::history_api::HistoryInfo;
use sequencer_core::l2_tx::{DirectInput, ValidUserOp};
use sequencer_core::user_op::UserOp;
use tokio_tungstenite::tungstenite::{self, Message};

use crate::engine::Engine;

const REQUEST_TIMEOUT: Duration = Duration::from_secs(10);
const SNAPSHOT_TIMEOUT: Duration = Duration::from_secs(600);
const PING_INTERVAL: Duration = Duration::from_secs(15);
const SILENCE_LIMIT: Duration = Duration::from_secs(45);
const BACKOFF_MIN: Duration = Duration::from_millis(250);
const BACKOFF_MAX: Duration = Duration::from_secs(15);
/// Consecutive restores that ended in divergence before the process gives up.
const MAX_DIVERGENCES: u32 = 3;

/// What the HTTP side reads.
#[derive(Default)]
pub struct Shared {
    pub engine: Mutex<Option<Engine>>,
    /// The application the sequencer serves, known after the first bootstrap.
    pub app: RwLock<Option<Address>>,
    /// The engine has applied the history the sequencer had when it was restored.
    pub ready: AtomicBool,
}

pub struct Config {
    /// `http://host:port` of the sequencer.
    pub sequencer_url: String,
    /// Where snapshot downloads are unpacked (removed once the engine has opened them).
    pub work_dir: PathBuf,
    pub max_snapshot_bytes: u64,
}

enum Stop {
    /// Transport trouble: keep the engine, try the same position again.
    Retry(String),
    /// The sequencer no longer serves our history: restore from a snapshot.
    Rebootstrap(String),
    /// Our state cannot be trusted (or a snapshot could not be opened).
    Diverged(String),
}

struct Backoff(Duration);

impl Backoff {
    fn reset(&mut self) {
        self.0 = BACKOFF_MIN;
    }

    async fn wait(&mut self) {
        tokio::time::sleep(self.0).await;
        self.0 = (self.0 * 2).min(BACKOFF_MAX);
    }
}

fn log(message: impl AsRef<str>) {
    eprintln!("qa-wallet-inspect: {}", message.as_ref());
}

fn discard(shared: &Shared) {
    shared.ready.store(false, Ordering::SeqCst);
    *shared.engine.lock().unwrap_or_else(|e| e.into_inner()) = None;
}

/// Runs until the follower cannot continue; the returned reason is fatal.
pub async fn run(shared: Arc<Shared>, config: Config) -> String {
    let http = match reqwest::Client::builder()
        .connect_timeout(REQUEST_TIMEOUT)
        .build()
    {
        Ok(http) => http,
        Err(e) => return format!("http client: {e}"),
    };
    let mut backoff = Backoff(BACKOFF_MIN);
    let mut divergences = 0u32;
    loop {
        discard(&shared);
        let stop = match bootstrap(&shared, &config, &http).await {
            Ok((version, target)) => {
                backoff.reset();
                follow(&shared, &config, version, target, &mut backoff).await
            }
            Err(stop) => stop,
        };
        discard(&shared);
        match stop {
            Stop::Retry(reason) => log(format!("bootstrap failed, retrying: {reason}")),
            Stop::Rebootstrap(reason) => {
                divergences = 0;
                log(format!("restoring from a new snapshot: {reason}"));
            }
            Stop::Diverged(reason) => {
                divergences += 1;
                log(format!("state discarded ({divergences}/{MAX_DIVERGENCES}): {reason}"));
                if divergences >= MAX_DIVERGENCES {
                    return reason;
                }
            }
        }
        backoff.wait().await;
    }
}

async fn follow(
    shared: &Shared,
    config: &Config,
    version: HistoryVersion,
    target: u64,
    backoff: &mut Backoff,
) -> Stop {
    loop {
        match session(shared, config, version, target, backoff).await {
            Stop::Retry(reason) => {
                log(format!("feed interrupted, reconnecting: {reason}"));
                backoff.wait().await;
            }
            stop => return stop,
        }
    }
}

// ---------------------------------------------------------------------------- bootstrap

/// A scratch directory that goes away with its owner, also when the follower is cancelled.
struct Scratch(PathBuf);

impl Drop for Scratch {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

async fn bootstrap(
    shared: &Shared,
    config: &Config,
    http: &reqwest::Client,
) -> Result<(HistoryVersion, u64), Stop> {
    let dir = scratch_dir(&config.work_dir).map_err(|e| Stop::Retry(format!("work dir: {e}")))?;
    restore(shared, config, http, &dir.0).await
}

async fn restore(
    shared: &Shared,
    config: &Config,
    http: &reqwest::Client,
    dir: &Path,
) -> Result<(HistoryVersion, u64), Stop> {
    let base = config.sequencer_url.trim_end_matches('/');
    let archive = dir.join("snapshot.tar");
    let claim = tokio::time::timeout(
        SNAPSHOT_TIMEOUT,
        download_snapshot(http, base, &archive, config.max_snapshot_bytes),
    )
    .await
    .map_err(|_| Stop::Retry("snapshot download timed out".into()))??;

    // The history the sequencer holds now, under the snapshot's version: the readiness target.
    let history = async {
        http.get(format!("{base}/history"))
            .timeout(REQUEST_TIMEOUT)
            .send()
            .await?
            .error_for_status()?
            .bytes()
            .await
    }
    .await
    .map_err(|e| Stop::Retry(format!("GET /history: {e}")))?;
    let history: HistoryInfo = serde_json::from_slice(&history)
        .map_err(|e| Stop::Retry(format!("GET /history: {e}")))?;
    if history.history.version != claim.version {
        return Err(Stop::Retry("history changed while the snapshot was downloading".into()));
    }

    let unpacked = dir.join("dump");
    let max = config.max_snapshot_bytes;
    let engine = tokio::task::spawn_blocking(move || -> Result<Engine, String> {
        unpack_snapshot(&archive, &unpacked, max).map_err(|e| format!("snapshot archive: {e}"))?;
        // The engine copies the state into its own working file (QA_ENGINE_WORKDIR).
        Engine::from_dump(&unpacked.join("state")).map_err(|e| {
            format!("opening the snapshot (does the QA_* layout match the sequencer's?): {e}")
        })
    })
    .await
    .map_err(|e| Stop::Diverged(format!("snapshot restore panicked: {e}")))?
    .map_err(Stop::Diverged)?;

    let count = engine.executed_input_count();
    if count != claim.next_input {
        return Err(Stop::Diverged(format!(
            "snapshot holds {} inputs but was served as {}",
            count.get(),
            claim.next_input.get()
        )));
    }
    let target = history.history.head.get();
    log(format!(
        "restored snapshot at input {} (era {}, generation {}); sequencer head {target}",
        count.get(),
        claim.version.era_id,
        claim.version.recovery_generation.get()
    ));
    *shared.app.write().unwrap_or_else(|e| e.into_inner()) = Some(history.deployment.app_address);
    *shared.engine.lock().unwrap_or_else(|e| e.into_inner()) = Some(engine);
    shared.ready.store(count.get() >= target, Ordering::SeqCst);
    Ok((claim.version, target))
}

fn scratch_dir(parent: &Path) -> io::Result<Scratch> {
    static NEXT: AtomicU64 = AtomicU64::new(0);
    let nanos = std::time::SystemTime::now()
        .duration_since(std::time::UNIX_EPOCH)
        .map_or(0, |d| d.as_nanos());
    let dir = parent.join(format!(
        "qa-wallet-inspect-{}-{nanos}-{}",
        std::process::id(),
        NEXT.fetch_add(1, Ordering::Relaxed)
    ));
    // create (not create_all): never adopt a directory someone else prepared
    fs::DirBuilder::new().mode(0o700).create(&dir)?;
    Ok(Scratch(dir))
}

async fn download_snapshot(
    http: &reqwest::Client,
    base: &str,
    dest: &Path,
    max_bytes: u64,
) -> Result<HistoryClaim, Stop> {
    use tokio::io::AsyncWriteExt;
    let retry = |what: &str, e: &dyn std::fmt::Display| Stop::Retry(format!("{what}: {e}"));

    let mut response = http
        .get(format!("{base}/latest_snapshot"))
        .send()
        .await
        .and_then(reqwest::Response::error_for_status)
        .map_err(|e| retry("GET /latest_snapshot", &e))?;
    let header = |name: &str| -> Result<String, Stop> {
        response
            .headers()
            .get(name)
            .and_then(|v| v.to_str().ok())
            .map(str::to_owned)
            .ok_or_else(|| Stop::Retry(format!("/latest_snapshot: missing {name}")))
    };
    let era_id: EraId = header("X-History-Era")?
        .parse()
        .map_err(|e| retry("X-History-Era", &e))?;
    let generation: u64 = header("X-Recovery-Generation")?
        .parse()
        .map_err(|e| retry("X-Recovery-Generation", &e))?;
    let count: u64 = header("X-Executed-Input-Count")?
        .parse()
        .map_err(|e| retry("X-Executed-Input-Count", &e))?;

    let mut file = tokio::fs::File::create(dest)
        .await
        .map_err(|e| retry("snapshot file", &e))?;
    let mut total = 0u64;
    // The body has no Content-Length: a short read is a failed download and fails to unpack.
    while let Some(chunk) = response
        .chunk()
        .await
        .map_err(|e| retry("snapshot body", &e))?
    {
        total += chunk.len() as u64;
        if total > max_bytes {
            return Err(Stop::Retry(format!(
                "snapshot exceeds --max-snapshot-bytes ({max_bytes})"
            )));
        }
        file.write_all(&chunk)
            .await
            .map_err(|e| retry("snapshot file", &e))?;
    }
    file.flush().await.map_err(|e| retry("snapshot file", &e))?;
    Ok(HistoryClaim {
        version: HistoryVersion {
            era_id,
            recovery_generation: RecoveryGeneration::new(generation),
        },
        next_input: ExecutedInputCount::new(count),
    })
}

/// Unpacks a sequencer snapshot archive into the new directory `dir`. Only the regular files the
/// sequencer puts at the top of a single-file dump are accepted, each at most once, and at most
/// `max_bytes` are written in total; `info.toml` and `state` must both be present.
pub fn unpack_snapshot(archive: &Path, dir: &Path, max_bytes: u64) -> io::Result<()> {
    const NAMES: [&str; 3] = ["info.toml", "state", "checkpoint.toml"];
    let bad = |reason: String| io::Error::new(io::ErrorKind::InvalidData, reason);

    fs::DirBuilder::new().mode(0o700).create(dir)?;
    let mut budget = max_bytes;
    let mut seen = [false; NAMES.len()];
    let mut tar = tar::Archive::new(File::open(archive)?);
    for entry in tar.entries()? {
        let mut entry = entry?;
        let path = entry.path()?.into_owned();
        let name = path.to_str().map(|p| p.strip_prefix("./").unwrap_or(p));
        let Some(index) = name.and_then(|n| NAMES.iter().position(|known| *known == n)) else {
            return Err(bad(format!("unexpected entry {}", path.display())));
        };
        let kind = entry.header().entry_type();
        if !(kind.is_file() || kind.is_gnu_sparse()) {
            return Err(bad(format!("{} is not a regular file", NAMES[index])));
        }
        if std::mem::replace(&mut seen[index], true) {
            return Err(bad(format!("duplicate entry {}", NAMES[index])));
        }
        let mut out = fs::OpenOptions::new()
            .write(true)
            .create_new(true)
            .open(dir.join(NAMES[index]))?;
        let mut buffer = vec![0u8; 1 << 16];
        loop {
            let n = entry.read(&mut buffer)?;
            if n == 0 {
                break;
            }
            budget = budget
                .checked_sub(n as u64)
                .ok_or_else(|| bad(format!("contents exceed {max_bytes} bytes")))?;
            out.write_all(&buffer[..n])?;
        }
    }
    if !(seen[0] && seen[1]) {
        return Err(bad("info.toml or state missing (a multi-file dump is not supported)".into()));
    }
    Ok(())
}

// ---------------------------------------------------------------------------- feed

fn subscribe_url(base: &str, claim: HistoryClaim) -> String {
    let ws = if let Some(rest) = base.strip_prefix("https://") {
        format!("wss://{rest}")
    } else if let Some(rest) = base.strip_prefix("http://") {
        format!("ws://{rest}")
    } else {
        format!("ws://{base}")
    };
    format!(
        "{}/ws/subscribe?era_id={}&recovery_generation={}&next_input={}",
        ws.trim_end_matches('/'),
        claim.version.era_id,
        claim.version.recovery_generation.get(),
        claim.next_input.get()
    )
}

/// One websocket connection, from the engine's current position until something stops it.
async fn session(
    shared: &Shared,
    config: &Config,
    version: HistoryVersion,
    target: u64,
    backoff: &mut Backoff,
) -> Stop {
    let next_input = match shared.engine.lock().unwrap_or_else(|e| e.into_inner()).as_ref() {
        Some(engine) => engine.executed_input_count(),
        None => return Stop::Diverged("engine missing".into()),
    };
    let url = subscribe_url(&config.sequencer_url, HistoryClaim { version, next_input });
    let connected = tokio::time::timeout(REQUEST_TIMEOUT, tokio_tungstenite::connect_async(url)).await;
    let mut socket = match connected {
        Err(_) => return Stop::Retry("subscribe timed out".into()),
        // 409: the claim is no longer part of the sequencer's history (X-History-Error says why)
        Ok(Err(tungstenite::Error::Http(response))) if response.status().as_u16() == 409 => {
            let why = response
                .headers()
                .get("X-History-Error")
                .and_then(|v| v.to_str().ok())
                .unwrap_or("history claim refused")
                .to_owned();
            return Stop::Rebootstrap(why);
        }
        Ok(Err(e)) => return Stop::Retry(format!("subscribe: {e}")),
        Ok(Ok((socket, _))) => socket,
    };

    // The server sends nothing while idle, so liveness is our pings being answered.
    let mut ping = tokio::time::interval(PING_INTERVAL);
    let mut last_seen = Instant::now();
    loop {
        tokio::select! {
            _ = ping.tick() => {
                if last_seen.elapsed() > SILENCE_LIMIT {
                    return Stop::Retry("sequencer stopped answering pings".into());
                }
                if let Err(e) = socket.send(Message::Ping(Vec::new().into())).await {
                    return Stop::Retry(format!("ping: {e}"));
                }
            }
            frame = socket.next() => {
                last_seen = Instant::now();
                let text = match frame {
                    None => return Stop::Retry("connection closed".into()),
                    Some(Err(e)) => return Stop::Retry(format!("read: {e}")),
                    Some(Ok(Message::Text(text))) => text,
                    Some(Ok(Message::Close(_))) => return Stop::Retry("closed by the sequencer".into()),
                    Some(Ok(_)) => continue, // pong (tungstenite answers pings itself)
                };
                let message: WsTxMessage = match serde_json::from_str(text.as_str()) {
                    Ok(message) => message,
                    Err(e) => return Stop::Diverged(format!("undecodable feed message: {e}")),
                };
                // Executing is blocking work (the engine syncs its state file).
                let applied = tokio::task::block_in_place(|| {
                    let mut guard = shared.engine.lock().unwrap_or_else(|e| e.into_inner());
                    let engine = guard.as_mut().ok_or("engine missing")?;
                    apply(engine, message)?;
                    Ok::<u64, String>(engine.executed_input_count().get())
                });
                match applied {
                    Ok(count) => {
                        backoff.reset();
                        if count >= target && !shared.ready.swap(true, Ordering::SeqCst) {
                            log(format!("caught up at input {count}"));
                        }
                    }
                    Err(reason) => return Stop::Diverged(reason),
                }
            }
        }
    }
}

fn decode_hex(field: &str, value: &str) -> Result<Vec<u8>, String> {
    let digits = value
        .strip_prefix("0x")
        .ok_or_else(|| format!("{field} is not 0x-prefixed"))?;
    hex::decode(digits).map_err(|e| format!("{field}: {e}"))
}

fn decode_address(value: &str) -> Result<Address, String> {
    let bytes = decode_hex("sender", value)?;
    Address::try_from(bytes.as_slice()).map_err(|_| format!("sender has {} bytes", bytes.len()))
}

/// Applies one feed message the way the sequencer's own replay does: the next offset only, with
/// exactly the persisted fields. The signed nonce is not an execution argument, so a user op is
/// first validated under it; the sequencer only includes ops that validate.
pub fn apply<A: Application>(app: &mut A, message: WsTxMessage) -> Result<(), String> {
    let expected = app.executed_input_count().get();
    if message.offset() != expected {
        return Err(format!(
            "feed gap: expected input {expected}, got {}",
            message.offset()
        ));
    }
    match message {
        WsTxMessage::DirectInput {
            sender,
            block_number,
            payload,
            ..
        } => {
            let input = DirectInput {
                sender: decode_address(&sender)?,
                block_number,
                payload: decode_hex("payload", &payload)?,
            };
            execute_direct_input(app, &input).map_err(|e| format!("input {expected}: {e}"))?;
        }
        WsTxMessage::UserOp {
            sender,
            nonce,
            fee,
            data,
            safe_block,
            ..
        } => {
            let sender = decode_address(&sender)?;
            let data = decode_hex("data", &data)?;
            let signed = UserOp {
                nonce,
                max_fee: fee,
                data: data.clone().into(),
            };
            match app.validate_user_op(sender, &signed, fee) {
                Ok(ValidationOutcome::Accept) => {}
                Ok(ValidationOutcome::Reject(reason)) => {
                    return Err(format!(
                        "input {expected}: user op from {sender} does not validate here: {reason:?}"
                    ));
                }
                Err(e) => return Err(format!("input {expected}: {e}")),
            }
            execute_valid_user_op(app, &ValidUserOp { sender, fee, data }, safe_block)
                .map_err(|e| format!("input {expected}: {e}"))?;
        }
    }
    Ok(())
}
