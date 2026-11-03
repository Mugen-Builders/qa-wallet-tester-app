// The sequencer host for qa-wallet: the generic C-application host of the Cartesi sequencer over
// the engine archive named by APPLICATION_ENGINE_LIB / APPLICATION_ENGINE_HEADER at build time
// (make seq-host). Subcommands, flags and environment are the sequencer's (docs/sequencer.md).
use std::process::ExitCode;

#[tokio::main]
async fn main() -> ExitCode {
    c_app_sequencer::run().await
}
