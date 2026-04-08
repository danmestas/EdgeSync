//! HTTP server running on a Unix socket.
//!
//! Exposes three endpoints:
//!   POST /exchange/{endpoint-id} — proxy xfer bytes to a remote peer over QUIC
//!   GET  /status                 — return local endpoint info as JSON
//!   POST /shutdown               — graceful shutdown

use std::{
    collections::HashMap,
    str::FromStr,
    sync::Arc,
};

use axum::{
    Router,
    body::Bytes,
    extract::{Path, State},
    http::StatusCode,
    response::{IntoResponse, Response},
    routing::{get, post},
};
use iroh::{
    EndpointAddr, EndpointId,
    endpoint::{Connection, Endpoint},
};
use serde_json::json;
use tokio::sync::{Mutex, oneshot};

/// Shared state injected into every axum handler.
#[derive(Clone)]
pub struct AppState {
    pub endpoint: Endpoint,
    pub endpoint_id: String,
    pub alpn: Vec<u8>,
    /// Local NATS server address for tunnel connections.
    pub nats_addr: Option<String>,
    /// Cache of live outbound connections keyed by remote endpoint-id string.
    pub conn_cache: Arc<Mutex<HashMap<String, Connection>>>,
    /// Send on this channel to trigger graceful shutdown.
    pub shutdown_tx: Arc<Mutex<Option<oneshot::Sender<()>>>>,
}

impl AppState {
    pub fn new(
        endpoint: Endpoint,
        endpoint_id: String,
        alpn: Vec<u8>,
        nats_addr: Option<String>,
        shutdown_tx: oneshot::Sender<()>,
    ) -> Self {
        AppState {
            endpoint,
            endpoint_id,
            alpn,
            nats_addr,
            conn_cache: Arc::new(Mutex::new(HashMap::new())),
            shutdown_tx: Arc::new(Mutex::new(Some(shutdown_tx))),
        }
    }
}

/// Build the axum Router with all routes wired up.
pub fn build_router(state: AppState) -> Router {
    Router::new()
        .route("/exchange/{endpoint_id}", post(handle_exchange))
        .route("/nats-tunnel/{endpoint_id}", post(handle_nats_tunnel))
        .route("/status", get(handle_status))
        .route("/shutdown", post(handle_shutdown))
        .with_state(state)
}

/// POST /exchange/{endpoint-id}
///
/// Connects to the remote peer (or reuses a cached connection), opens a
/// bidirectional QUIC stream, writes the request body, then streams back the
/// full response.
async fn handle_exchange(
    State(state): State<AppState>,
    Path(remote_id_str): Path<String>,
    body: Bytes,
) -> Result<Response, AppError> {
    let remote_id = EndpointId::from_str(&remote_id_str)
        .map_err(|e| AppError::bad_request(format!("invalid endpoint id: {e}")))?;

    let conn = get_or_connect(&state, &remote_id_str, remote_id).await?;

    let (mut send, mut recv) = conn.open_bi().await.map_err(AppError::internal)?;

    send.write_all(&body).await.map_err(AppError::internal)?;
    send.finish().map_err(AppError::internal)?;

    let response_bytes = recv
        .read_to_end(50 * 1024 * 1024)
        .await
        .map_err(AppError::internal)?;

    Ok((StatusCode::OK, response_bytes).into_response())
}

/// POST /nats-tunnel/{endpoint-id}
///
/// Establishes an outbound NATS leaf node tunnel to a remote peer. Connects
/// over QUIC using the NATS ALPN, then pipes bidirectionally between the
/// local NATS server and the remote peer's NATS server. Returns 200
/// immediately; the tunnel runs in a background task.
async fn handle_nats_tunnel(
    State(state): State<AppState>,
    Path(remote_id_str): Path<String>,
) -> Result<Response, AppError> {
    let nats_addr = state.nats_addr.clone().ok_or_else(|| {
        AppError::bad_request("NATS tunnel not available: --nats-addr not configured")
    })?;

    let remote_id = EndpointId::from_str(&remote_id_str)
        .map_err(|e| AppError::bad_request(format!("invalid endpoint id: {e}")))?;

    tracing::info!(remote = %remote_id_str, %nats_addr, "opening outbound NATS tunnel");

    let addr: EndpointAddr = remote_id.into();
    let conn = state
        .endpoint
        .connect(addr, crate::NATS_ALPN)
        .await
        .map_err(AppError::internal)?;

    tokio::spawn(async move {
        crate::tunnel::establish_outbound(conn, nats_addr).await;
    });

    Ok(StatusCode::OK.into_response())
}

/// GET /status
async fn handle_status(State(state): State<AppState>) -> impl IntoResponse {
    let addr = state.endpoint.addr();
    let relay_url = addr
        .relay_urls()
        .next()
        .map(|u: &iroh::RelayUrl| u.to_string())
        .unwrap_or_default();

    axum::Json(json!({
        "endpoint_id": state.endpoint_id,
        "relay_url": relay_url,
    }))
}

/// POST /shutdown
async fn handle_shutdown(State(state): State<AppState>) -> impl IntoResponse {
    let mut guard = state.shutdown_tx.lock().await;
    if let Some(tx) = guard.take() {
        let _ = tx.send(());
    }
    StatusCode::OK
}

/// Retrieve a live connection from the cache, or open a new one.
///
/// Holds the lock across the connect call to prevent duplicate connections
/// for the same peer from concurrent requests.
async fn get_or_connect(
    state: &AppState,
    remote_id_str: &str,
    remote_id: EndpointId,
) -> Result<Connection, AppError> {
    let mut cache = state.conn_cache.lock().await;

    if let Some(conn) = cache.get(remote_id_str) {
        if conn.close_reason().is_none() {
            tracing::debug!(remote = %remote_id_str, "reusing cached connection");
            return Ok(conn.clone());
        }
        tracing::debug!(remote = %remote_id_str, "cached connection is closed, reconnecting");
        cache.remove(remote_id_str);
    }

    tracing::info!(remote = %remote_id_str, "connecting to remote peer");
    let addr: EndpointAddr = remote_id.into();
    let conn: Connection = state
        .endpoint
        .connect(addr, &state.alpn)
        .await
        .map_err(|e| AppError::internal(e))?;

    cache.insert(remote_id_str.to_string(), conn.clone());

    Ok(conn)
}

// ---------------------------------------------------------------------------
// Error helper
// ---------------------------------------------------------------------------

struct AppError {
    status: StatusCode,
    message: String,
}

impl AppError {
    fn bad_request(msg: impl Into<String>) -> Self {
        AppError {
            status: StatusCode::BAD_REQUEST,
            message: msg.into(),
        }
    }

    fn internal(err: impl std::fmt::Display) -> Self {
        AppError {
            status: StatusCode::INTERNAL_SERVER_ERROR,
            message: err.to_string(),
        }
    }
}

impl IntoResponse for AppError {
    fn into_response(self) -> Response {
        (self.status, self.message).into_response()
    }
}

// ---------------------------------------------------------------------------
// Server entry point
// ---------------------------------------------------------------------------

/// Bind the Unix socket and serve until `shutdown_rx` fires.
pub async fn run_server(
    socket_path: &std::path::Path,
    state: AppState,
    shutdown_rx: oneshot::Receiver<()>,
) -> anyhow::Result<()> {
    // Remove stale socket file.
    if socket_path.exists() {
        std::fs::remove_file(socket_path)?;
    }

    let listener = tokio::net::UnixListener::bind(socket_path)?;
    tracing::info!(socket = %socket_path.display(), "HTTP server listening on Unix socket");

    let router = build_router(state);

    axum::serve(listener, router)
        .with_graceful_shutdown(async move {
            let _ = shutdown_rx.await;
            tracing::info!("HTTP server received shutdown signal");
        })
        .await?;

    Ok(())
}
