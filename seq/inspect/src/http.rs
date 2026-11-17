//! The inspect route, in the shape the Cartesi rollups node serves it: the request body is the
//! raw inspect payload, the answer carries the reports as hex.

use std::sync::Arc;
use std::sync::atomic::Ordering;

use alloy_primitives::{Address, hex};
use axum::body::Bytes;
use axum::extract::{DefaultBodyLimit, Path, State};
use axum::http::StatusCode;
use axum::response::{IntoResponse, Response};
use axum::routing::{get, post};
use axum::{Json, Router};
use sequencer_core::application::Application;
use serde_json::{Value, json};
use tokio::sync::Semaphore;

use crate::follow::Shared;

/// The machine's input limit: nothing larger could be an inspect payload.
pub const MAX_PAYLOAD_BYTES: usize = 2 << 20;
/// Requests held (body included) at once; the engine answers them one at a time.
const MAX_IN_FLIGHT: usize = 16;

#[derive(Clone)]
struct AppState {
    shared: Arc<Shared>,
    in_flight: Arc<Semaphore>,
}

pub fn router(shared: Arc<Shared>) -> Router {
    Router::new()
        .route("/inspect/{application}", post(inspect))
        .layer(DefaultBodyLimit::max(MAX_PAYLOAD_BYTES))
        .route("/healthz", get(|| async { StatusCode::OK }))
        .route("/readyz", get(readyz))
        .with_state(AppState {
            shared,
            in_flight: Arc::new(Semaphore::new(MAX_IN_FLIGHT)),
        })
}

fn error(status: StatusCode, message: &str) -> Response {
    (status, Json(json!({ "error": message }))).into_response()
}

async fn readyz(State(state): State<AppState>) -> StatusCode {
    if state.shared.ready.load(Ordering::SeqCst) {
        StatusCode::OK
    } else {
        StatusCode::SERVICE_UNAVAILABLE
    }
}

pub fn inspect_response(reports: &[Vec<u8>], processed_input_count: u64) -> Value {
    json!({
        "status": "Accepted",
        "exception_payload": null,
        "reports": reports
            .iter()
            .map(|report| json!({ "payload": hex::encode_prefixed(report) }))
            .collect::<Vec<_>>(),
        "processed_input_count": processed_input_count,
    })
}

async fn inspect(
    State(state): State<AppState>,
    Path(application): Path<String>,
    payload: Bytes,
) -> Response {
    let Ok(_permit) = state.in_flight.clone().try_acquire_owned() else {
        return error(StatusCode::TOO_MANY_REQUESTS, "too many inspects in flight");
    };
    let Some(app) = *state.shared.app.read().unwrap_or_else(|e| e.into_inner()) else {
        return error(StatusCode::SERVICE_UNAVAILABLE, "not synchronised with the sequencer yet");
    };
    if application.parse::<Address>().ok() != Some(app) {
        return error(StatusCode::NOT_FOUND, "unknown application");
    }
    let shared = state.shared.clone();
    let answer = tokio::task::spawn_blocking(move || {
        let mut guard = shared.engine.lock().unwrap_or_else(|e| e.into_inner());
        // ready is only ever set while an engine is present, and cleared before it is dropped
        let engine = guard
            .as_mut()
            .filter(|_| shared.ready.load(Ordering::SeqCst))?;
        let reports = engine.inspect(&payload);
        Some((reports, engine.executed_input_count().get()))
    })
    .await;
    match answer {
        Ok(Some((Ok(reports), count))) => Json(inspect_response(&reports, count)).into_response(),
        Ok(Some((Err(e), _))) => error(StatusCode::INTERNAL_SERVER_ERROR, &e.to_string()),
        Ok(None) => error(StatusCode::SERVICE_UNAVAILABLE, "not synchronised with the sequencer yet"),
        Err(_) => error(StatusCode::INTERNAL_SERVER_ERROR, "inspect failed"),
    }
}
