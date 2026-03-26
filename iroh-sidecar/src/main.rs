mod endpoint;

use clap::Parser;
use std::path::PathBuf;

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

    // Placeholder — HTTP server and accept loop added in subsequent tasks.
    tokio::signal::ctrl_c().await?;
    ep.close().await;
    Ok(())
}
