// The inspect follower for qa-wallet: restores the sequencer's latest snapshot into its own engine
// (the archive named by APPLICATION_ENGINE_LIB / APPLICATION_ENGINE_HEADER at build time,
// make seq-inspect), keeps it current from the sequencer's transaction feed, and answers inspect
// payloads from that state (docs/sequencer.md). The engine reads the QA_* layout from the
// environment: it must be the sequencer's.
mod engine;
mod follow;
mod http;
#[cfg(test)]
mod tests;

use std::net::SocketAddr;
use std::path::PathBuf;
use std::process::ExitCode;
use std::sync::Arc;

use clap::Parser;

#[derive(Parser)]
#[command(name = "qa-wallet-inspect", about = "Inspect server following a qa-wallet sequencer")]
struct Args {
    /// The sequencer's HTTP endpoint (http://host:port).
    #[arg(long, env = "QA_INSPECT_SEQUENCER_URL")]
    sequencer_url: String,
    /// Where inspects are served.
    #[arg(long, env = "QA_INSPECT_HTTP_ADDR", default_value = "127.0.0.1:3001")]
    http_addr: SocketAddr,
    /// Scratch directory for snapshot downloads [default: QA_ENGINE_WORKDIR, else the system's]
    #[arg(long, env = "QA_INSPECT_WORK_DIR")]
    work_dir: Option<PathBuf>,
    /// Largest snapshot archive accepted from the sequencer, in bytes.
    #[arg(long, env = "QA_INSPECT_MAX_SNAPSHOT_BYTES", default_value_t = 1 << 30)]
    max_snapshot_bytes: u64,
}

async fn shutdown_signal() {
    use tokio::signal::unix::{SignalKind, signal};
    let mut terminate = signal(SignalKind::terminate()).expect("SIGTERM handler");
    tokio::select! {
        _ = tokio::signal::ctrl_c() => {}
        _ = terminate.recv() => {}
    }
}

#[tokio::main]
async fn main() -> ExitCode {
    let args = Args::parse();
    if !args.sequencer_url.starts_with("http://") {
        eprintln!("qa-wallet-inspect: --sequencer-url must be an http:// URL");
        return ExitCode::FAILURE;
    }
    let work_dir = args
        .work_dir
        .or_else(|| std::env::var_os("QA_ENGINE_WORKDIR").map(PathBuf::from))
        .unwrap_or_else(std::env::temp_dir);
    let listener = match tokio::net::TcpListener::bind(args.http_addr).await {
        Ok(listener) => listener,
        Err(e) => {
            eprintln!("qa-wallet-inspect: cannot listen on {}: {e}", args.http_addr);
            return ExitCode::FAILURE;
        }
    };

    let shared = Arc::new(follow::Shared::default());
    let mut follower = tokio::spawn(follow::run(
        shared.clone(),
        follow::Config {
            sequencer_url: args.sequencer_url,
            work_dir,
            max_snapshot_bytes: args.max_snapshot_bytes,
        },
    ));
    let server = axum::serve(listener, http::router(shared.clone()))
        .with_graceful_shutdown(shutdown_signal());
    eprintln!("qa-wallet-inspect: serving inspects on {}", args.http_addr);

    let code = tokio::select! {
        served = server => {
            follower.abort();
            let _ = follower.await;
            match served {
                Ok(()) => ExitCode::SUCCESS,
                Err(e) => {
                    eprintln!("qa-wallet-inspect: http server: {e}");
                    ExitCode::FAILURE
                }
            }
        }
        // the follower only returns (or panics) when it cannot go on
        stopped = &mut follower => {
            match stopped {
                Ok(reason) => eprintln!("qa-wallet-inspect: giving up: {reason}"),
                Err(e) => eprintln!("qa-wallet-inspect: follower failed: {e}"),
            }
            ExitCode::FAILURE
        }
    };
    // Dropping the engine removes its working copy.
    *shared.engine.lock().unwrap_or_else(|e| e.into_inner()) = None;
    code
}
