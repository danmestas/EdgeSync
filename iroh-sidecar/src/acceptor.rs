//! Accept loop for incoming QUIC connections from remote peers.
//!
//! When a remote leaf agent opens a QUIC stream on our ALPN we:
//!   1. Accept the connection and the first bidirectional stream.
//!   2. Read the full request body from the stream.
//!   3. Forward it to the Go agent via HTTP POST to the callback URL.
//!   4. Write the Go response bytes back on the QUIC stream.

use iroh::Endpoint;
use tokio::task::JoinSet;

use crate::NATS_ALPN;

/// Run the accept loop until the endpoint is closed.
///
/// Each incoming connection is handled in a tracked task so we can await
/// all in-flight work on shutdown rather than killing tasks mid-request.
pub async fn run_accept_loop(
    endpoint: Endpoint,
    callback_url: String,
    nats_addr: Option<String>,
) {
    let client = reqwest::Client::new();
    let mut tasks = JoinSet::new();

    loop {
        // `endpoint.accept()` returns None when the endpoint is closed.
        let incoming = match endpoint.accept().await {
            Some(i) => i,
            None => {
                tracing::info!("accept loop: endpoint closed, waiting for in-flight tasks");
                break;
            }
        };

        let client = client.clone();
        let callback_url = callback_url.clone();
        let nats_addr = nats_addr.clone();

        tasks.spawn(async move {
            if let Err(e) = handle_incoming(incoming, client, callback_url, nats_addr).await {
                tracing::warn!("accept loop: error handling incoming connection: {e:#}");
            }
        });

        // Reap completed tasks to avoid unbounded growth.
        while let Some(result) = tasks.try_join_next() {
            if let Err(e) = result {
                tracing::warn!("accept loop: task panicked: {e:#}");
            }
        }
    }

    // Await all remaining in-flight tasks before returning.
    while let Some(result) = tasks.join_next().await {
        if let Err(e) = result {
            tracing::warn!("accept loop: task panicked during shutdown: {e:#}");
        }
    }
    tracing::info!("accept loop: all tasks completed");
}

async fn handle_incoming(
    incoming: iroh::endpoint::Incoming,
    client: reqwest::Client,
    callback_url: String,
    nats_addr: Option<String>,
) -> anyhow::Result<()> {
    let connection = incoming.accept()?.await?;
    let remote_id = connection.remote_id();
    let alpn = connection.alpn();
    tracing::info!(%remote_id, alpn = %String::from_utf8_lossy(alpn), "accepted incoming connection");

    // Route NATS tunnel connections to the tunnel module.
    if alpn == NATS_ALPN {
        let nats_addr = nats_addr.ok_or_else(|| {
            anyhow::anyhow!("received NATS tunnel connection but --nats-addr is not configured")
        })?;
        crate::tunnel::handle_connection(connection, nats_addr).await;
        return Ok(());
    }

    // Otherwise handle as xfer (existing behavior).
    let mut stream_tasks = JoinSet::new();

    // Each new stream on this connection is a separate request.
    loop {
        let (mut send, mut recv) = match connection.accept_bi().await {
            Ok(streams) => streams,
            Err(e) => {
                tracing::debug!(%remote_id, "stream accept ended: {e}");
                break;
            }
        };

        let client = client.clone();
        let callback_url = callback_url.clone();

        stream_tasks.spawn(async move {
            if let Err(e) =
                handle_stream(&mut send, &mut recv, &client, &callback_url, remote_id).await
            {
                tracing::warn!(%remote_id, "stream handler error: {e:#}");
            }
        });
    }

    // Await all in-flight stream handlers before closing the connection.
    while let Some(result) = stream_tasks.join_next().await {
        if let Err(e) = result {
            tracing::warn!(%remote_id, "stream task panicked: {e:#}");
        }
    }

    Ok(())
}

async fn handle_stream(
    send: &mut iroh::endpoint::SendStream,
    recv: &mut iroh::endpoint::RecvStream,
    client: &reqwest::Client,
    callback_url: &str,
    remote_id: iroh::EndpointId,
) -> anyhow::Result<()> {
    // Read request bytes (up to 50 MB).
    let request_bytes = recv.read_to_end(50 * 1024 * 1024).await?;
    tracing::debug!(
        %remote_id,
        bytes = request_bytes.len(),
        "received xfer request"
    );

    // Forward to the Go agent's HTTP handler.
    let response = client
        .post(callback_url)
        .header("Content-Type", "application/x-fossil")
        .body(request_bytes)
        .send()
        .await?;

    let status = response.status();
    let response_bytes = response.bytes().await?;

    tracing::debug!(
        %remote_id,
        http_status = %status,
        bytes = response_bytes.len(),
        "received callback response"
    );

    // Write response back to the peer.
    send.write_all(&response_bytes).await?;
    send.finish()?;

    Ok(())
}
