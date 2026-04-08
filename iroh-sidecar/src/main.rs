mod acceptor;
mod endpoint;
mod server;
mod tunnel;

use clap::Parser;
use std::path::PathBuf;
use tokio::sync::oneshot;

#[derive(Parser)]
#[command(name = "iroh-sidecar", about = "Iroh P2P proxy for EdgeSync")]
struct Args {
    /// Unix socket path for the HTTP API
    #[arg(long)]
    socket: PathBuf,

    /// Path to persist the Ed25519 keypair
    #[arg(long)]
    key_path: PathBuf,

    /// URL to forward incoming peer requests to (Go agent's HTTP handler)
    #[arg(long)]
    callback: String,

    /// ALPN protocol identifier
    #[arg(long, default_value = "/edgesync/xfer/1")]
    alpn: String,
}

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt::init();
    let args = Args::parse();
    tracing::info!(socket = %args.socket.display(), "iroh-sidecar starting");

    let (ep, endpoint_id) = endpoint::create_endpoint(
        &args.key_path,
        args.alpn.as_bytes(),
    ).await?;

    tracing::info!(%endpoint_id, "iroh endpoint ready");

    // Channel used by POST /shutdown to stop the HTTP server.
    let (shutdown_tx, shutdown_rx) = oneshot::channel::<()>();

    let state = server::AppState::new(
        ep.clone(),
        endpoint_id,
        args.alpn.as_bytes().to_vec(),
        shutdown_tx,
    );

    // Task 4: Accept loop for incoming P2P connections.
    let accept_ep = ep.clone();
    let callback_url = args.callback.clone();
    let accept_handle = tokio::spawn(async move {
        acceptor::run_accept_loop(accept_ep, callback_url).await;
    });

    // Task 3: HTTP server on the Unix socket.
    let socket_path = args.socket.clone();
    let http_handle = tokio::spawn(async move {
        if let Err(e) = server::run_server(&socket_path, state, shutdown_rx).await {
            tracing::error!("HTTP server error: {e:#}");
        }
    });

    // Wait for either task to finish.
    // The HTTP server exits when it receives /shutdown; the accept loop exits
    // when the endpoint is closed.
    tokio::select! {
        _ = http_handle => {
            tracing::info!("HTTP server exited");
        }
        _ = accept_handle => {
            tracing::info!("accept loop exited unexpectedly");
        }
        _ = tokio::signal::ctrl_c() => {
            tracing::info!("received ctrl-c");
        }
    }

    ep.close().await;
    tracing::info!("iroh-sidecar stopped");
    Ok(())
}
