//! Bidirectional TCP-QUIC pipe for NATS leaf node connections.
//!
//! The NATS leaf node protocol is asymmetric:
//!   - **Hub side** sends INFO, waits for CONNECT from the leaf.
//!   - **Leaf side** sends CONNECT in response to INFO.
//!
//! Inbound (accepting) side: the sidecar connects TCP to the local NATS leaf
//! port, which acts as hub — correct.
//!
//! Outbound (soliciting) side: the sidecar binds a local TCP listener and
//! returns its port. The Go mesh configures the embedded NATS server to solicit
//! a leaf connection to that port. The sidecar pipes the resulting TCP
//! connection through QUIC to the remote hub.

use iroh::endpoint::Connection;
use tokio::io::AsyncWriteExt;
use tokio::net::{TcpListener, TcpStream};

/// Handle an inbound connection on the NATS tunnel ALPN.
///
/// Accepts bidirectional streams in a loop and pipes each one to a fresh
/// TCP connection to `nats_addr` (the local NATS leaf node port, hub side).
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
            if let Err(e) = pipe_to_nats(send, recv, &nats_addr).await {
                tracing::warn!(%remote_id, "nats tunnel: inbound pipe error: {e:#}");
            }
        });
    }
}

/// Establish an outbound NATS tunnel.
///
/// Binds `127.0.0.1:0` (ephemeral port) and returns the port. The caller
/// configures the embedded NATS server to solicit a leaf connection to this
/// port. When NATS connects, the sidecar opens a QUIC bi-stream to the remote
/// peer and pipes bytes bidirectionally.
pub async fn establish_outbound(connection: Connection) -> anyhow::Result<u16> {
    let listener = TcpListener::bind("127.0.0.1:0").await?;
    let port = listener.local_addr()?.port();
    let remote_id = connection.remote_id();

    tracing::info!(%remote_id, %port, "nats tunnel: outbound listener ready");

    tokio::spawn(async move {
        // Wait for local NATS to connect (it will solicit to this port).
        let (tcp_stream, _) = match listener.accept().await {
            Ok(s) => s,
            Err(e) => {
                tracing::warn!(%remote_id, "nats tunnel: accept failed: {e}");
                return;
            }
        };

        // Open QUIC bi-stream to remote peer.
        let (send, recv) = match connection.open_bi().await {
            Ok(s) => s,
            Err(e) => {
                tracing::warn!(%remote_id, "nats tunnel: open_bi failed: {e}");
                return;
            }
        };

        // Pipe: local NATS (leaf) <-> QUIC <-> remote NATS (hub)
        if let Err(e) = pipe_tcp_to_quic(tcp_stream, send, recv).await {
            tracing::warn!(%remote_id, "nats tunnel: outbound pipe error: {e:#}");
        }
    });

    Ok(port)
}

/// Pipe bytes bidirectionally between a TCP stream and a QUIC stream pair.
async fn pipe_tcp_to_quic(
    tcp: TcpStream,
    mut quic_send: iroh::endpoint::SendStream,
    mut quic_recv: iroh::endpoint::RecvStream,
) -> anyhow::Result<()> {
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

/// Connect TCP to a local NATS address and pipe through QUIC streams.
/// Used by the inbound (hub) side where we connect to the local NATS leaf port.
async fn pipe_to_nats(
    quic_send: iroh::endpoint::SendStream,
    quic_recv: iroh::endpoint::RecvStream,
    nats_addr: &str,
) -> anyhow::Result<()> {
    let tcp = TcpStream::connect(nats_addr).await?;
    pipe_tcp_to_quic(tcp, quic_send, quic_recv).await
}
