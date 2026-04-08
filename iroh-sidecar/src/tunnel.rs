//! Bidirectional TCP-QUIC pipe for NATS leaf node connections.
//!
//! Each QUIC bi-stream is paired with a fresh TCP connection to the local
//! NATS server. Bytes flow in both directions until either side closes.

use iroh::endpoint::Connection;
use tokio::io::AsyncWriteExt;
use tokio::net::TcpStream;

/// Handle an inbound connection on the NATS tunnel ALPN.
///
/// Accepts bidirectional streams in a loop and pipes each one to a fresh
/// TCP connection to `nats_addr`.
pub async fn handle_connection(connection: Connection, nats_addr: String) {
    let remote_id = connection.remote_id();
    tracing::info!(%remote_id, %nats_addr, "nats tunnel: accepted inbound connection");

    loop {
        let (send, recv) = match connection.accept_bi().await {
            Ok(streams) => streams,
            Err(e) => {
                tracing::debug!(%remote_id, "nats tunnel: stream accept ended: {e}");
                break;
            }
        };

        let nats_addr = nats_addr.clone();
        tokio::spawn(async move {
            if let Err(e) = pipe_stream(send, recv, &nats_addr).await {
                tracing::warn!(%remote_id, "nats tunnel: inbound pipe error: {e:#}");
            }
        });
    }
}

/// Establish an outbound NATS tunnel stream.
///
/// Opens one bidirectional stream on the given connection and pipes it to a
/// fresh TCP connection to `nats_addr`.
pub async fn establish_outbound(connection: Connection, nats_addr: String) {
    let remote_id = connection.remote_id();
    tracing::info!(%remote_id, %nats_addr, "nats tunnel: opening outbound stream");

    let (send, recv) = match connection.open_bi().await {
        Ok(streams) => streams,
        Err(e) => {
            tracing::warn!(%remote_id, "nats tunnel: failed to open bi stream: {e:#}");
            return;
        }
    };

    if let Err(e) = pipe_stream(send, recv, &nats_addr).await {
        tracing::warn!(%remote_id, "nats tunnel: outbound pipe error: {e:#}");
    }
}

/// Pipe bytes bidirectionally between a QUIC stream pair and a TCP connection
/// to the local NATS server.
async fn pipe_stream(
    mut quic_send: iroh::endpoint::SendStream,
    mut quic_recv: iroh::endpoint::RecvStream,
    nats_addr: &str,
) -> anyhow::Result<()> {
    let tcp = TcpStream::connect(nats_addr).await?;
    let (mut tcp_read, mut tcp_write) = tcp.into_split();

    // QUIC recv -> TCP write
    let q2t = tokio::spawn(async move {
        if let Err(e) = tokio::io::copy(&mut quic_recv, &mut tcp_write).await {
            tracing::debug!("nats tunnel: quic->tcp copy ended: {e}");
        }
        // Shut down the TCP write half so the NATS server sees EOF.
        let _ = tcp_write.shutdown().await;
    });

    // TCP read -> QUIC send
    let t2q = tokio::spawn(async move {
        // We need an AsyncRead adapter; TcpStream's read half implements it.
        let mut buf = vec![0u8; 32 * 1024];
        loop {
            let n = match tokio::io::AsyncReadExt::read(&mut tcp_read, &mut buf).await {
                Ok(0) => break,
                Ok(n) => n,
                Err(e) => {
                    tracing::debug!("nats tunnel: tcp->quic read error: {e}");
                    break;
                }
            };
            if let Err(e) = quic_send.write_all(&buf[..n]).await {
                tracing::debug!("nats tunnel: tcp->quic write error: {e}");
                break;
            }
        }
        let _ = quic_send.finish();
    });

    // Wait for both copy loops to finish.
    let _ = tokio::join!(q2t, t2q);

    Ok(())
}
