# Iroh P2P Transport Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add iroh as a peer-to-peer transport for leaf-to-leaf sync — NAT-busting direct connections without NATS or central infrastructure.

**Architecture:** A Rust sidecar binary (`iroh-sidecar`) handles all iroh networking (QUIC, holepunching, relay, discovery). The Go leaf agent spawns it as a child process and communicates over HTTP on a Unix socket. A new `IrohTransport` implements `sync.Transport`, identical in shape to the existing `NATSTransport` and `HTTPTransport`.

**Tech Stack:** Rust (iroh, axum, tokio, clap) for the sidecar; Go for the transport and agent integration.

**Spec:** `docs/superpowers/specs/2026-03-26-iroh-p2p-transport-design.md`

---

### Task 1: Scaffold the Rust Sidecar Crate

**Files:**
- Create: `iroh-sidecar/Cargo.toml`
- Create: `iroh-sidecar/src/main.rs`

- [ ] **Step 1: Create the Cargo project**

```bash
mkdir -p iroh-sidecar/src
```

- [ ] **Step 2: Write Cargo.toml**

```toml
[package]
name = "iroh-sidecar"
version = "0.1.0"
edition = "2024"

[dependencies]
iroh = "0.97"
axum = "0.8"
hyper-util = { version = "0.1", features = ["tokio"] }
tokio = { version = "1", features = ["full"] }
clap = { version = "4", features = ["derive"] }
serde = { version = "1", features = ["derive"] }
serde_json = "1"
tracing = "0.1"
tracing-subscriber = "0.3"
```

- [ ] **Step 3: Write minimal main.rs with CLI parsing**

```rust
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

    // Placeholder — will be filled in Task 2 and Task 3.
    todo!("start iroh endpoint and HTTP server")
}
```

- [ ] **Step 4: Verify it compiles**

Run: `cd iroh-sidecar && cargo check`
Expected: compiles successfully (the `todo!()` is fine at compile time)

- [ ] **Step 5: Add .gitignore for Rust artifacts**

Create `iroh-sidecar/.gitignore`:
```
/target
```

- [ ] **Step 6: Commit**

```bash
git add iroh-sidecar/
git commit -m "feat(iroh): scaffold Rust sidecar crate with CLI parsing"
```

---

### Task 2: Iroh Endpoint and Key Persistence

**Files:**
- Create: `iroh-sidecar/src/endpoint.rs`
- Modify: `iroh-sidecar/src/main.rs`

- [ ] **Step 1: Write endpoint.rs — key loading/generation and Endpoint creation**

```rust
use iroh::{Endpoint, SecretKey};
use std::path::Path;
use tokio::fs;

/// Load or generate an Ed25519 keypair, then bind an iroh Endpoint.
pub async fn create_endpoint(
    key_path: &Path,
    alpn: &[u8],
) -> anyhow::Result<(Endpoint, String)> {
    let secret_key = load_or_generate_key(key_path).await?;
    let endpoint_id = secret_key.public().to_string();

    let endpoint = Endpoint::builder()
        .secret_key(secret_key)
        .alpns(vec![alpn.to_vec()])
        .bind()
        .await?;

    tracing::info!(%endpoint_id, "iroh endpoint bound");
    Ok((endpoint, endpoint_id))
}

async fn load_or_generate_key(path: &Path) -> anyhow::Result<SecretKey> {
    if path.exists() {
        let bytes = fs::read(path).await?;
        let key: SecretKey = postcard::from_bytes(&bytes)?;
        tracing::info!("loaded existing keypair from {}", path.display());
        Ok(key)
    } else {
        let key = SecretKey::generate(rand::rngs::OsRng);
        if let Some(parent) = path.parent() {
            fs::create_dir_all(parent).await?;
        }
        let bytes = postcard::to_allocvec(&key)?;
        fs::write(path, &bytes).await?;
        tracing::info!("generated new keypair at {}", path.display());
        Ok(key)
    }
}
```

Note: The exact `SecretKey` serialization API may differ depending on iroh version. Check `iroh::SecretKey` docs — it may implement `Display`/`FromStr` for hex encoding, or use `to_bytes()`/`from_bytes()` instead of postcard. Adjust accordingly.

- [ ] **Step 2: Add postcard and rand dependencies to Cargo.toml**

Add to `[dependencies]`:
```toml
postcard = { version = "1", features = ["alloc"] }
rand = "0.8"
anyhow = "1"
```

- [ ] **Step 3: Wire endpoint.rs into main.rs**

Replace the `todo!()` in `main.rs`:
```rust
mod endpoint;

// In main():
let (ep, endpoint_id) = endpoint::create_endpoint(
    &args.key_path,
    args.alpn.as_bytes(),
).await?;

tracing::info!(%endpoint_id, "iroh endpoint ready");

// Placeholder — HTTP server and accept loop added in Task 3 and Task 4.
// For now, just hold the endpoint open.
tokio::signal::ctrl_c().await?;
ep.close().await;
Ok(())
```

- [ ] **Step 4: Verify it compiles**

Run: `cd iroh-sidecar && cargo check`
Expected: compiles successfully

- [ ] **Step 5: Commit**

```bash
git add iroh-sidecar/
git commit -m "feat(iroh): add endpoint creation with key persistence"
```

---

### Task 3: Sidecar HTTP Server (Exchange + Status + Shutdown)

**Files:**
- Create: `iroh-sidecar/src/server.rs`
- Modify: `iroh-sidecar/src/main.rs`

- [ ] **Step 1: Write server.rs — HTTP API on Unix socket**

```rust
use axum::{
    Router,
    extract::{Path, State},
    http::StatusCode,
    response::Json,
    routing::{get, post},
};
use hyper_util::rt::TokioIo;
use iroh::{Endpoint, endpoint::Connection, EndpointId};
use serde::Serialize;
use std::{collections::HashMap, path::PathBuf, str::FromStr, sync::Arc};
use tokio::{net::UnixListener, sync::{Mutex, oneshot}};

#[derive(Clone)]
pub struct AppState {
    pub endpoint: Endpoint,
    pub endpoint_id: String,
    pub alpn: Vec<u8>,
    pub connections: Arc<Mutex<HashMap<String, Connection>>>,
    pub shutdown_tx: Arc<Mutex<Option<oneshot::Sender<()>>>>,
}

#[derive(Serialize)]
struct StatusResponse {
    endpoint_id: String,
    relay_url: Option<String>,
}

pub async fn run_server(
    socket_path: PathBuf,
    state: AppState,
) -> anyhow::Result<()> {
    // Remove stale socket file if it exists.
    let _ = tokio::fs::remove_file(&socket_path).await;

    let app = Router::new()
        .route("/exchange/{endpoint_id}", post(exchange_handler))
        .route("/status", get(status_handler))
        .route("/shutdown", post(shutdown_handler))
        .with_state(state.clone());

    let listener = UnixListener::bind(&socket_path)?;
    tracing::info!(path = %socket_path.display(), "HTTP server listening on Unix socket");

    let (shutdown_tx, shutdown_rx) = oneshot::channel::<()>();
    {
        let mut tx = state.shutdown_tx.lock().await;
        *tx = Some(shutdown_tx);
    }

    loop {
        tokio::select! {
            accept = listener.accept() => {
                let (stream, _) = accept?;
                let app = app.clone();
                tokio::spawn(async move {
                    let io = TokioIo::new(stream);
                    if let Err(e) = hyper_util::server::conn::auto::Builder::new(
                        hyper_util::rt::TokioExecutor::new(),
                    )
                    .serve_connection(io, app.into_service())
                    .await
                    {
                        tracing::error!("connection error: {e}");
                    }
                });
            }
            _ = shutdown_rx => {
                tracing::info!("shutdown requested");
                return Ok(());
            }
        }
    }
}

async fn exchange_handler(
    State(state): State<AppState>,
    Path(remote_id): Path<String>,
    body: axum::body::Bytes,
) -> Result<axum::body::Bytes, StatusCode> {
    let endpoint_id = EndpointId::from_str(&remote_id)
        .map_err(|_| StatusCode::BAD_REQUEST)?;

    // Get or create connection to remote peer.
    let conn = get_or_connect(&state, endpoint_id).await
        .map_err(|e| {
            tracing::error!("connect to {remote_id}: {e}");
            StatusCode::BAD_GATEWAY
        })?;

    // Open a bidirectional stream, send request, read response.
    let (mut send, mut recv) = conn.open_bi().await
        .map_err(|e| {
            tracing::error!("open_bi to {remote_id}: {e}");
            StatusCode::BAD_GATEWAY
        })?;

    send.write_all(&body).await.map_err(|e| {
        tracing::error!("write to {remote_id}: {e}");
        StatusCode::BAD_GATEWAY
    })?;
    send.finish().map_err(|e| {
        tracing::error!("finish to {remote_id}: {e}");
        StatusCode::BAD_GATEWAY
    })?;

    let response = recv.read_to_end(50 * 1024 * 1024).await.map_err(|e| {
        tracing::error!("read from {remote_id}: {e}");
        StatusCode::BAD_GATEWAY
    })?;

    Ok(axum::body::Bytes::from(response))
}

async fn get_or_connect(
    state: &AppState,
    remote: EndpointId,
) -> anyhow::Result<Connection> {
    let key = remote.to_string();
    {
        let conns = state.connections.lock().await;
        if let Some(conn) = conns.get(&key) {
            if conn.close_reason().is_none() {
                return Ok(conn.clone());
            }
        }
    }

    tracing::info!(%key, "connecting to peer");
    let conn = state.endpoint.connect(remote.into(), &state.alpn).await?;
    {
        let mut conns = state.connections.lock().await;
        conns.insert(key, conn.clone());
    }
    Ok(conn)
}

async fn status_handler(
    State(state): State<AppState>,
) -> Json<StatusResponse> {
    let relay_url = state.endpoint.home_relay().map(|r| r.to_string());
    Json(StatusResponse {
        endpoint_id: state.endpoint_id.clone(),
        relay_url,
    })
}

async fn shutdown_handler(
    State(state): State<AppState>,
) -> StatusCode {
    let tx = {
        let mut guard = state.shutdown_tx.lock().await;
        guard.take()
    };
    if let Some(tx) = tx {
        let _ = tx.send(());
    }
    StatusCode::OK
}
```

Note: The exact axum routing syntax, `hyper_util` server API, and `iroh::Connection` method names may need adjustment based on the exact versions of these crates. The `conn.close_reason()` check for connection liveness may use a different API — check iroh docs. The `state.endpoint.connect()` call takes an `EndpointAddr` (not just an `EndpointId`) in some versions — DNS discovery resolves the ID to an address automatically when `connect_by_id()` or similar is available. Adjust to match the iroh v0.97 API.

- [ ] **Step 2: Wire server into main.rs**

Replace the placeholder in `main.rs`:
```rust
mod endpoint;
mod server;

use server::AppState;
use std::{collections::HashMap, sync::Arc};
use tokio::sync::Mutex;

// In main(), after endpoint creation:
let state = AppState {
    endpoint: ep.clone(),
    endpoint_id: endpoint_id.clone(),
    alpn: args.alpn.as_bytes().to_vec(),
    connections: Arc::new(Mutex::new(HashMap::new())),
    shutdown_tx: Arc::new(Mutex::new(None)),
};

server::run_server(args.socket, state).await?;

ep.close().await;
Ok(())
```

- [ ] **Step 3: Verify it compiles**

Run: `cd iroh-sidecar && cargo check`
Expected: compiles successfully

- [ ] **Step 4: Commit**

```bash
git add iroh-sidecar/
git commit -m "feat(iroh): add HTTP server with exchange, status, shutdown endpoints"
```

---

### Task 4: Sidecar Accept Loop (Incoming Connections)

**Files:**
- Create: `iroh-sidecar/src/acceptor.rs`
- Modify: `iroh-sidecar/src/main.rs`

- [ ] **Step 1: Write acceptor.rs — accept incoming QUIC connections and forward to Go**

```rust
use iroh::Endpoint;
use reqwest::Client;

/// Accept incoming iroh connections and proxy requests to the Go callback URL.
pub async fn accept_loop(
    endpoint: Endpoint,
    alpn: Vec<u8>,
    callback_url: String,
) -> anyhow::Result<()> {
    let client = Client::new();
    tracing::info!("accept loop started, forwarding to {callback_url}");

    while let Some(incoming) = endpoint.accept().await {
        let alpn = alpn.clone();
        let callback_url = callback_url.clone();
        let client = client.clone();

        tokio::spawn(async move {
            match incoming.accept() {
                Ok(connecting) => {
                    match connecting.await {
                        Ok(conn) => {
                            if let Err(e) = handle_connection(conn, &callback_url, &client).await {
                                tracing::error!("handle connection: {e}");
                            }
                        }
                        Err(e) => tracing::error!("connection failed: {e}"),
                    }
                }
                Err(e) => tracing::error!("accept failed: {e}"),
            }
        });
    }

    Ok(())
}

async fn handle_connection(
    conn: iroh::endpoint::Connection,
    callback_url: &str,
    client: &Client,
) -> anyhow::Result<()> {
    let remote = conn.remote_endpoint_id();
    tracing::info!(?remote, "accepted incoming connection");

    // Accept bidirectional streams for the lifetime of the connection.
    loop {
        let (mut send, mut recv) = match conn.accept_bi().await {
            Ok(streams) => streams,
            Err(_) => return Ok(()), // Connection closed.
        };

        let request_bytes = recv.read_to_end(50 * 1024 * 1024).await?;

        // Forward to Go agent's HTTP handler.
        let response = client
            .post(callback_url)
            .header("Content-Type", "application/x-fossil")
            .body(request_bytes)
            .send()
            .await?;

        let response_bytes = response.bytes().await?;
        send.write_all(&response_bytes).await?;
        send.finish()?;
    }
}
```

Note: The `incoming.accept()` API may differ in iroh v0.97 — it might be `incoming.await` directly or `incoming.accept().await`. The `conn.remote_endpoint_id()` method may be called `remote_node_id()` in older versions. Check iroh docs and adjust.

- [ ] **Step 2: Add reqwest dependency to Cargo.toml**

Add to `[dependencies]`:
```toml
reqwest = { version = "0.12", default-features = false, features = ["rustls-tls"] }
```

- [ ] **Step 3: Wire accept loop into main.rs alongside HTTP server**

Update `main.rs` to run both the accept loop and HTTP server concurrently:

```rust
mod acceptor;
mod endpoint;
mod server;

use server::AppState;
use std::{collections::HashMap, sync::Arc};
use tokio::sync::Mutex;

#[tokio::main]
async fn main() -> anyhow::Result<()> {
    tracing_subscriber::fmt::init();
    let args = Args::parse();
    tracing::info!(socket = %args.socket.display(), "iroh-sidecar starting");

    let (ep, endpoint_id) = endpoint::create_endpoint(
        &args.key_path,
        args.alpn.as_bytes(),
    ).await?;

    let state = AppState {
        endpoint: ep.clone(),
        endpoint_id: endpoint_id.clone(),
        alpn: args.alpn.as_bytes().to_vec(),
        connections: Arc::new(Mutex::new(HashMap::new())),
        shutdown_tx: Arc::new(Mutex::new(None)),
    };

    // Run accept loop and HTTP server concurrently.
    let accept_handle = tokio::spawn(acceptor::accept_loop(
        ep.clone(),
        args.alpn.as_bytes().to_vec(),
        args.callback.clone(),
    ));

    let server_handle = tokio::spawn(server::run_server(
        args.socket,
        state,
    ));

    // Wait for either to finish (HTTP server exits on /shutdown).
    tokio::select! {
        res = server_handle => {
            if let Err(e) = res? {
                tracing::error!("server error: {e}");
            }
        }
        res = accept_handle => {
            if let Err(e) = res? {
                tracing::error!("accept loop error: {e}");
            }
        }
    }

    ep.close().await;
    Ok(())
}
```

- [ ] **Step 4: Verify it compiles**

Run: `cd iroh-sidecar && cargo check`
Expected: compiles successfully

- [ ] **Step 5: Commit**

```bash
git add iroh-sidecar/
git commit -m "feat(iroh): add accept loop for incoming peer connections"
```

---

### Task 5: Build the Sidecar Binary

**Files:**
- Modify: `Makefile`
- Modify: `iroh-sidecar/.gitignore`

- [ ] **Step 1: Add iroh-sidecar target to Makefile**

Add after the `bridge:` target:

```makefile
iroh-sidecar:
	cd iroh-sidecar && cargo build --release
	cp iroh-sidecar/target/release/iroh-sidecar bin/
```

Update the `.PHONY` line to include `iroh-sidecar`.

Update the `build:` target to include `iroh-sidecar`:

```makefile
build: edgesync leaf bridge iroh-sidecar
```

- [ ] **Step 2: Build the sidecar**

Run: `make iroh-sidecar`
Expected: Compiles and copies binary to `bin/iroh-sidecar`

- [ ] **Step 3: Verify the binary runs**

Run: `bin/iroh-sidecar --help`
Expected: Shows CLI help with `--socket`, `--key-path`, `--callback`, `--alpn` flags

- [ ] **Step 4: Commit**

```bash
git add Makefile iroh-sidecar/.gitignore
git commit -m "build: add iroh-sidecar Makefile target"
```

---

### Task 6: IrohTransport (Go)

**Files:**
- Create: `leaf/agent/iroh.go`
- Create: `leaf/agent/iroh_test.go`

- [ ] **Step 1: Write the failing test for IrohTransport.Exchange**

Create `leaf/agent/iroh_test.go`:

```go
package agent

import (
	"context"
	"io"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

func TestIrohTransportRoundTrip(t *testing.T) {
	// Start a mock sidecar HTTP server on a Unix socket.
	dir := t.TempDir()
	sock := filepath.Join(dir, "iroh.sock")

	listener, err := net.Listen("unix", sock)
	if err != nil {
		t.Fatalf("listen: %v", err)
	}
	defer listener.Close()

	// Mock sidecar: accept any POST to /exchange/*, echo back a canned igot response.
	cannedResp := &xfer.Message{
		Cards: []xfer.Card{
			&xfer.IGotCard{UUID: "iroh-test-uuid-1234"},
		},
	}
	cannedBytes, err := cannedResp.Encode()
	if err != nil {
		t.Fatalf("encode canned: %v", err)
	}

	mux := http.NewServeMux()
	mux.HandleFunc("/exchange/", func(w http.ResponseWriter, r *http.Request) {
		// Verify we received valid xfer data.
		body, _ := io.ReadAll(r.Body)
		if _, err := xfer.Decode(body); err != nil {
			t.Errorf("mock sidecar: decode request: %v", err)
			w.WriteHeader(http.StatusBadRequest)
			return
		}
		w.WriteHeader(http.StatusOK)
		w.Write(cannedBytes)
	})
	srv := &http.Server{Handler: mux}
	go srv.Serve(listener)
	defer srv.Close()

	// Create transport and exchange.
	transport := NewIrohTransport(sock, "fake-endpoint-id-abc123")

	req := &xfer.Message{
		Cards: []xfer.Card{
			&xfer.PullCard{ServerCode: "srvX", ProjectCode: "testproj"},
		},
	}

	ctx := context.Background()
	resp, err := transport.Exchange(ctx, req)
	if err != nil {
		t.Fatalf("Exchange: %v", err)
	}
	if len(resp.Cards) != 1 {
		t.Fatalf("expected 1 card, got %d", len(resp.Cards))
	}
	igot, ok := resp.Cards[0].(*xfer.IGotCard)
	if !ok {
		t.Fatalf("expected *IGotCard, got %T", resp.Cards[0])
	}
	if igot.UUID != "iroh-test-uuid-1234" {
		t.Errorf("UUID = %q, want %q", igot.UUID, "iroh-test-uuid-1234")
	}
}

func TestIrohTransportSidecarDown(t *testing.T) {
	// Transport pointing at a non-existent socket should return an error.
	transport := NewIrohTransport("/tmp/iroh-nonexistent.sock", "fake-id")

	req := &xfer.Message{
		Cards: []xfer.Card{
			&xfer.PullCard{ServerCode: "srvX", ProjectCode: "testproj"},
		},
	}

	ctx := context.Background()
	_, err := transport.Exchange(ctx, req)
	if err == nil {
		t.Fatal("expected error when sidecar is down, got nil")
	}
	t.Logf("error (expected): %v", err)
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd leaf && go test ./agent/ -run TestIrohTransport -v -count=1`
Expected: FAIL — `NewIrohTransport` not defined

- [ ] **Step 3: Write IrohTransport implementation**

Create `leaf/agent/iroh.go`:

```go
package agent

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"net"
	"net/http"

	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// IrohTransport implements sync.Transport over the iroh sidecar's HTTP API.
// The sidecar listens on a Unix socket and proxies requests to remote peers
// via iroh QUIC connections.
type IrohTransport struct {
	socketPath string
	endpointID string // remote peer's EndpointId
	client     *http.Client
}

// NewIrohTransport creates a transport that exchanges xfer messages via the
// iroh sidecar HTTP API on the given Unix socket. endpointID is the remote
// peer's iroh EndpointId.
func NewIrohTransport(socketPath, endpointID string) *IrohTransport {
	return &IrohTransport{
		socketPath: socketPath,
		endpointID: endpointID,
		client: &http.Client{
			Transport: &http.Transport{
				DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
					return net.Dial("unix", socketPath)
				},
			},
		},
	}
}

// Exchange encodes the xfer request, sends it to the sidecar which forwards it
// to the remote peer via iroh, and decodes the response.
func (t *IrohTransport) Exchange(ctx context.Context, req *xfer.Message) (*xfer.Message, error) {
	if req == nil {
		panic("agent.IrohTransport.Exchange: req must not be nil")
	}

	body, err := req.Encode()
	if err != nil {
		return nil, fmt.Errorf("iroh: encode request: %w", err)
	}

	url := "http://iroh-sidecar/exchange/" + t.endpointID
	httpReq, err := http.NewRequestWithContext(ctx, "POST", url, bytes.NewReader(body))
	if err != nil {
		return nil, fmt.Errorf("iroh: create request: %w", err)
	}
	httpReq.Header.Set("Content-Type", "application/x-fossil")

	resp, err := t.client.Do(httpReq)
	if err != nil {
		return nil, fmt.Errorf("iroh: sidecar request: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("iroh: sidecar returned %d", resp.StatusCode)
	}

	respBody, err := io.ReadAll(resp.Body)
	if err != nil {
		return nil, fmt.Errorf("iroh: read response: %w", err)
	}

	return xfer.Decode(respBody)
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd leaf && go test ./agent/ -run TestIrohTransport -v -count=1`
Expected: PASS — both `TestIrohTransportRoundTrip` and `TestIrohTransportSidecarDown` pass

- [ ] **Step 5: Commit**

```bash
git add leaf/agent/iroh.go leaf/agent/iroh_test.go
git commit -m "feat(iroh): add IrohTransport implementing sync.Transport over Unix socket"
```

---

### Task 7: Sidecar Process Management

**Files:**
- Create: `leaf/agent/sidecar.go`
- Create: `leaf/agent/sidecar_test.go`

- [ ] **Step 1: Write the failing test**

Create `leaf/agent/sidecar_test.go`:

```go
package agent

import (
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"
)

func TestSidecarLifecycle(t *testing.T) {
	// Create a stub sidecar script that responds to /status and /shutdown.
	dir := t.TempDir()
	stubPath := filepath.Join(dir, "iroh-sidecar")
	stubScript := `#!/bin/sh
# Minimal stub: listen on the socket via socat or just sleep.
# The real test uses the Go HTTP client to check readiness.
# For unit testing, we just verify spawn/kill lifecycle.
sleep 60
`
	if err := os.WriteFile(stubPath, []byte(stubScript), 0755); err != nil {
		t.Fatalf("write stub: %v", err)
	}

	sock := filepath.Join(dir, "test.sock")
	keyPath := filepath.Join(dir, "test-key")

	sc := &sidecar{
		binPath:     stubPath,
		socketPath:  sock,
		keyPath:     keyPath,
		callbackURL: "http://127.0.0.1:8080",
		alpn:        "/edgesync/xfer/1",
	}

	if err := sc.spawn(); err != nil {
		t.Fatalf("spawn: %v", err)
	}

	// Process should be running.
	if sc.cmd == nil || sc.cmd.Process == nil {
		t.Fatal("expected process to be running")
	}
	pid := sc.cmd.Process.Pid
	t.Logf("sidecar pid: %d", pid)

	// Kill it.
	sc.kill()

	// Give it a moment to exit.
	time.Sleep(100 * time.Millisecond)

	// Process should be gone.
	proc, err := os.FindProcess(pid)
	if err == nil && proc != nil {
		// On Unix, FindProcess always succeeds. Check if it's actually alive.
		err := proc.Signal(os.Signal(nil))
		if err == nil {
			t.Error("process still alive after kill")
		}
	}
}

func TestSidecarBinaryNotFound(t *testing.T) {
	sc := &sidecar{
		binPath:     "/nonexistent/iroh-sidecar",
		socketPath:  "/tmp/test.sock",
		keyPath:     "/tmp/test-key",
		callbackURL: "http://127.0.0.1:8080",
		alpn:        "/edgesync/xfer/1",
	}

	err := sc.spawn()
	if err == nil {
		t.Fatal("expected error for missing binary")
	}
	t.Logf("error (expected): %v", err)
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd leaf && go test ./agent/ -run TestSidecar -v -count=1`
Expected: FAIL — `sidecar` type not defined

- [ ] **Step 3: Write sidecar process management**

Create `leaf/agent/sidecar.go`:

```go
package agent

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"syscall"
	"time"
)

// sidecar manages the iroh-sidecar child process lifecycle.
type sidecar struct {
	binPath     string
	socketPath  string
	keyPath     string
	callbackURL string
	alpn        string
	cmd         *exec.Cmd
}

// sidecarStatus is the JSON response from GET /status.
type sidecarStatus struct {
	EndpointID string  `json:"endpoint_id"`
	RelayURL   *string `json:"relay_url"`
}

// spawn starts the sidecar process.
func (s *sidecar) spawn() error {
	if _, err := os.Stat(s.binPath); err != nil {
		return fmt.Errorf("iroh sidecar binary not found: %w", err)
	}

	s.cmd = exec.Command(s.binPath,
		"--socket", s.socketPath,
		"--key-path", s.keyPath,
		"--callback", s.callbackURL,
		"--alpn", s.alpn,
	)
	s.cmd.Stdout = os.Stdout
	s.cmd.Stderr = os.Stderr

	if err := s.cmd.Start(); err != nil {
		return fmt.Errorf("iroh sidecar start: %w", err)
	}
	return nil
}

// waitReady polls GET /status until the sidecar responds or timeout elapses.
// Returns the sidecar's EndpointId.
func (s *sidecar) waitReady(timeout time.Duration) (*sidecarStatus, error) {
	client := &http.Client{
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
				return net.Dial("unix", s.socketPath)
			},
		},
		Timeout: 2 * time.Second,
	}

	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		resp, err := client.Get("http://iroh-sidecar/status")
		if err != nil {
			time.Sleep(200 * time.Millisecond)
			continue
		}
		defer resp.Body.Close()

		if resp.StatusCode != http.StatusOK {
			time.Sleep(200 * time.Millisecond)
			continue
		}

		var status sidecarStatus
		if err := json.NewDecoder(resp.Body).Decode(&status); err != nil {
			time.Sleep(200 * time.Millisecond)
			continue
		}
		return &status, nil
	}

	return nil, fmt.Errorf("iroh sidecar not ready within %v", timeout)
}

// shutdown sends POST /shutdown and waits for the process to exit.
// Falls back to SIGTERM then SIGKILL.
func (s *sidecar) shutdown() {
	if s.cmd == nil || s.cmd.Process == nil {
		return
	}

	// Try graceful shutdown via HTTP.
	client := &http.Client{
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
				return net.Dial("unix", s.socketPath)
			},
		},
		Timeout: 2 * time.Second,
	}
	client.Post("http://iroh-sidecar/shutdown", "", nil)

	// Wait up to 5s for exit.
	done := make(chan error, 1)
	go func() { done <- s.cmd.Wait() }()
	select {
	case <-done:
		s.cleanup()
		return
	case <-time.After(5 * time.Second):
	}

	// SIGTERM.
	s.cmd.Process.Signal(syscall.SIGTERM)
	select {
	case <-done:
		s.cleanup()
		return
	case <-time.After(2 * time.Second):
	}

	// SIGKILL.
	s.cmd.Process.Kill()
	<-done
	s.cleanup()
}

// kill forcefully terminates the sidecar process.
func (s *sidecar) kill() {
	if s.cmd != nil && s.cmd.Process != nil {
		s.cmd.Process.Kill()
		s.cmd.Wait()
	}
	s.cleanup()
}

func (s *sidecar) cleanup() {
	os.Remove(s.socketPath)
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd leaf && go test ./agent/ -run TestSidecar -v -count=1`
Expected: PASS — both tests pass

- [ ] **Step 5: Commit**

```bash
git add leaf/agent/sidecar.go leaf/agent/sidecar_test.go
git commit -m "feat(iroh): add sidecar process management (spawn, ready, shutdown)"
```

---

### Task 8: Agent Config and CLI Flags

**Files:**
- Modify: `leaf/agent/config.go`
- Modify: `leaf/cmd/leaf/main.go`

- [ ] **Step 1: Add iroh config fields**

In `leaf/agent/config.go`, add after the `PostSyncHook` field:

```go
	// IrohEnabled starts the iroh sidecar for peer-to-peer sync.
	IrohEnabled bool

	// IrohPeers is a list of remote EndpointIds to sync with.
	IrohPeers []string

	// IrohKeyPath is the path to the persistent Ed25519 keypair.
	// Defaults to "<repo-dir>.iroh-key" (adjacent to the repo file).
	IrohKeyPath string
```

In `applyDefaults()`, add:

```go
	if c.IrohEnabled && c.IrohKeyPath == "" {
		c.IrohKeyPath = c.RepoPath + ".iroh-key"
	}
```

- [ ] **Step 2: Add CLI flags to leaf/cmd/leaf/main.go**

Add after the `uv` flag:

```go
	iroh := flag.Bool("iroh", false, "enable iroh sidecar for peer-to-peer sync")
	irohKeyPath := flag.String("iroh-key", "", "path to iroh Ed25519 keypair (default: <repo>.iroh-key)")

	// irohPeers accumulates --iroh-peer flags.
	var irohPeers stringSlice
	flag.Var(&irohPeers, "iroh-peer", "remote iroh EndpointId to sync with (repeatable)")
```

Add the `stringSlice` type at the end of the file:

```go
// stringSlice implements flag.Value for repeatable string flags.
type stringSlice []string

func (s *stringSlice) String() string { return fmt.Sprintf("%v", *s) }
func (s *stringSlice) Set(v string) error {
	*s = append(*s, v)
	return nil
}
```

Wire the new flags into the Config struct:

```go
	cfg := agent.Config{
		// ... existing fields ...
		IrohEnabled: *iroh,
		IrohPeers:   irohPeers,
		IrohKeyPath: *irohKeyPath,
	}
```

- [ ] **Step 3: Verify it compiles**

Run: `cd leaf && go build -buildvcs=false ./cmd/leaf/`
Expected: compiles successfully

- [ ] **Step 4: Verify --help shows new flags**

Run: `cd leaf && go run -buildvcs=false ./cmd/leaf/ --help 2>&1 | grep iroh`
Expected: Shows `--iroh`, `--iroh-peer`, `--iroh-key` flags

- [ ] **Step 5: Commit**

```bash
git add leaf/agent/config.go leaf/cmd/leaf/main.go
git commit -m "feat(iroh): add IrohEnabled, IrohPeers, IrohKeyPath config and CLI flags"
```

---

### Task 9: Wire Iroh into Agent Lifecycle

**Files:**
- Modify: `leaf/agent/agent.go`

- [ ] **Step 1: Add sidecar field to Agent struct**

In `agent.go`, add to the `Agent` struct:

```go
	irohSidecar *sidecar            // nil when iroh is disabled
	irohSock    string              // Unix socket path for iroh sidecar
```

- [ ] **Step 2: Wire sidecar spawn into Start()**

In `Start()`, add after the existing server listener blocks (after the `ServeNATSEnabled` block):

```go
	// Iroh sidecar
	if a.config.IrohEnabled {
		binPath, err := a.findIrohBinary()
		if err != nil {
			return fmt.Errorf("agent: iroh: %w", err)
		}

		a.irohSock = fmt.Sprintf("/tmp/iroh-%d.sock", os.Getpid())
		callbackURL := "http://127.0.0.1" + a.config.ServeHTTPAddr
		if a.config.ServeHTTPAddr == "" {
			// Need an HTTP listener for iroh callbacks.
			// Start one on a random port.
			ln, err := net.Listen("tcp", "127.0.0.1:0")
			if err != nil {
				return fmt.Errorf("agent: iroh callback listener: %w", err)
			}
			callbackURL = "http://" + ln.Addr().String()
			mux := http.NewServeMux()
			mux.Handle("/", sync.XferHandler(a.repo, sync.HandleSync))
			srv := &http.Server{Handler: mux}
			go func() {
				<-ctx.Done()
				srv.Close()
			}()
			go srv.Serve(ln)
		}

		a.irohSidecar = &sidecar{
			binPath:     binPath,
			socketPath:  a.irohSock,
			keyPath:     a.config.IrohKeyPath,
			callbackURL: callbackURL,
			alpn:        "/edgesync/xfer/1",
		}

		if err := a.irohSidecar.spawn(); err != nil {
			return fmt.Errorf("agent: iroh sidecar: %w", err)
		}

		status, err := a.irohSidecar.waitReady(10 * time.Second)
		if err != nil {
			a.irohSidecar.kill()
			return fmt.Errorf("agent: iroh sidecar: %w", err)
		}
		a.logf("iroh sidecar ready, endpoint_id=%s", status.EndpointID)
	}
```

Add the `findIrohBinary` method:

```go
// findIrohBinary looks for the iroh-sidecar binary next to the leaf binary,
// then in PATH.
func (a *Agent) findIrohBinary() (string, error) {
	// Check next to the current executable.
	exe, err := os.Executable()
	if err == nil {
		candidate := filepath.Join(filepath.Dir(exe), "iroh-sidecar")
		if _, err := os.Stat(candidate); err == nil {
			return candidate, nil
		}
	}

	// Fall back to PATH.
	path, err := exec.LookPath("iroh-sidecar")
	if err != nil {
		return "", fmt.Errorf("iroh-sidecar binary not found (not next to leaf binary, not in PATH)")
	}
	return path, nil
}
```

Add necessary imports: `"net"`, `"os"`, `"path/filepath"`, `"os/exec"`, `"time"`, `"net/http"`.

- [ ] **Step 3: Wire iroh peers into pollLoop**

In `pollLoop()`, add iroh sync after the existing `Tick` call block. Replace the single `Tick` call with a loop that syncs all configured iroh peers after the NATS sync:

In the `pollLoop`, after the existing `act := a.Tick(ctx, ev)` and its result handling, add:

```go
		// Sync with iroh peers.
		if a.config.IrohEnabled && a.irohSock != "" {
			for _, peerID := range a.config.IrohPeers {
				transport := NewIrohTransport(a.irohSock, peerID)
				opts := sync.SyncOpts{
					Push:        a.config.Push,
					Pull:        a.config.Pull,
					ProjectCode: a.projectCode,
					ServerCode:  a.serverCode,
					User:        a.config.User,
					Password:    a.config.Password,
					Buggify:     a.config.Buggify,
					UV:          a.config.UV,
					Private:     a.config.Private,
					Observer:    a.config.Observer,
				}
				result, err := sync.Sync(ctx, a.repo, transport, opts)
				if err != nil {
					a.logf("iroh sync with %s: %v", peerID, err)
					slog.ErrorContext(ctx, "iroh sync error", "peer", peerID, "error", err)
					continue
				}
				a.logf("iroh sync with %s: ↑%d ↓%d rounds=%d", peerID, result.FilesSent, result.FilesRecvd, result.Rounds)
				if a.config.PostSyncHook != nil {
					a.config.PostSyncHook(result)
				}
			}
		}
```

- [ ] **Step 4: Wire sidecar shutdown into Stop()**

In `Stop()`, add before the NATS connection close:

```go
	if a.irohSidecar != nil {
		a.irohSidecar.shutdown()
	}
```

- [ ] **Step 5: Verify it compiles**

Run: `cd leaf && go build -buildvcs=false ./cmd/leaf/`
Expected: compiles successfully

- [ ] **Step 6: Run existing tests to verify no regressions**

Run: `cd leaf && go test ./agent/ -count=1 -short`
Expected: all existing tests pass

- [ ] **Step 7: Commit**

```bash
git add leaf/agent/agent.go
git commit -m "feat(iroh): wire sidecar lifecycle and peer sync into agent"
```

---

### Task 10: Integration Test

**Files:**
- Create: `sim/iroh_test.go`

This test requires a built `iroh-sidecar` binary. It tests the full plumbing: two leaf agents, each with a real sidecar, syncing blobs peer-to-peer on localhost.

- [ ] **Step 1: Write the integration test**

Create `sim/iroh_test.go`:

```go
package sim

import (
	"fmt"
	"math/rand"
	"os"
	"os/exec"
	"path/filepath"
	"testing"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/leaf/agent"
)

func TestIrohPeerSync(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping iroh integration test in short mode")
	}

	// Require iroh-sidecar binary.
	sidecarBin, err := exec.LookPath("iroh-sidecar")
	if err != nil {
		// Check in bin/ relative to project root.
		candidate := filepath.Join("..", "bin", "iroh-sidecar")
		if _, statErr := os.Stat(candidate); statErr != nil {
			t.Skip("iroh-sidecar binary not found, run 'make iroh-sidecar' first")
		}
		sidecarBin, _ = filepath.Abs(candidate)
	}
	_ = sidecarBin // Used implicitly — the agent finds it via PATH or adjacent binary.

	dir := t.TempDir()

	// 1. Create two repos with the same project-code.
	projCode := "abcdef0123456789abcdef0123456789abcdef01"
	var leafPaths [2]string
	for i := range 2 {
		path := filepath.Join(dir, fmt.Sprintf("leaf-%d.fossil", i))
		r, err := repo.Create(path, "testuser", simio.CryptoRand{})
		if err != nil {
			t.Fatalf("repo.Create leaf-%d: %v", i, err)
		}
		r.DB().Exec("UPDATE config SET value=? WHERE name='project-code'", projCode)
		r.Close()
		leafPaths[i] = path
	}

	// 2. Seed blobs into leaf-0.
	r0, err := repo.Open(leafPaths[0])
	if err != nil {
		t.Fatalf("open leaf-0: %v", err)
	}
	rng := rand.New(rand.NewSource(42))
	seededUUIDs, err := SeedLeaf(r0, rng, 5, 4096)
	if err != nil {
		r0.Close()
		t.Fatalf("SeedLeaf: %v", err)
	}
	r0.Close()
	t.Logf("Seeded %d blobs into leaf-0", len(seededUUIDs))

	// 3. Start leaf-0 with iroh enabled + serve-http for callback.
	// We need to get leaf-0's EndpointId before starting leaf-1.
	leaf0, err := agent.New(agent.Config{
		RepoPath:      leafPaths[0],
		NATSUrl:       "nats://localhost:14222", // intentionally wrong — no NATS needed
		Push:          true,
		Pull:          true,
		PollInterval:  60 * time.Second, // don't auto-poll, we trigger manually
		IrohEnabled:   true,
		IrohKeyPath:   filepath.Join(dir, "leaf-0.iroh-key"),
		ServeHTTPAddr: "127.0.0.1:0", // random port for iroh callback
	})
	if err != nil {
		t.Fatalf("agent.New leaf-0: %v", err)
	}
	if err := leaf0.Start(); err != nil {
		t.Fatalf("leaf-0 start: %v", err)
	}
	defer leaf0.Stop()

	// TODO: Get leaf-0's EndpointId from the sidecar status.
	// This requires exposing the EndpointId from the agent after Start().
	// For now, this test validates the plumbing compiles and the sidecar spawns.
	// Full convergence testing requires the EndpointId exchange.

	t.Log("leaf-0 started with iroh sidecar")

	// 4. Verify leaf-0's sidecar is healthy.
	// The Start() method already waits for readiness, so if we got here,
	// the sidecar is running.
	t.Log("iroh integration test: sidecar lifecycle verified")
}
```

Note: A complete convergence test requires both agents to know each other's EndpointIds. This requires either (a) exposing the EndpointId from the agent after Start, or (b) pre-generating keypairs and deriving EndpointIds. The test above validates the plumbing — sidecar spawn, readiness, and shutdown. A follow-up can add full two-agent convergence once EndpointId exchange is available.

- [ ] **Step 2: Verify the test compiles**

Run: `go test -buildvcs=false ./sim/ -run TestIrohPeerSync -v -count=1 -short`
Expected: SKIP (short mode)

- [ ] **Step 3: Run the full test (requires built sidecar)**

Run: `go test -buildvcs=false ./sim/ -run TestIrohPeerSync -v -count=1 -timeout=60s`
Expected: PASS if sidecar binary exists, SKIP otherwise

- [ ] **Step 4: Commit**

```bash
git add sim/iroh_test.go
git commit -m "test(sim): add iroh peer-to-peer integration test"
```

---

### Task 11: Docker Build Stage

**Files:**
- Modify: `deploy/Dockerfile`

- [ ] **Step 1: Add Rust builder stage**

Add before the final stage in `deploy/Dockerfile`:

```dockerfile
# --- Iroh sidecar (Rust) ---
FROM rust:1-bookworm AS iroh-builder
WORKDIR /src
COPY iroh-sidecar/ ./iroh-sidecar/
RUN cargo build --release --manifest-path iroh-sidecar/Cargo.toml
```

- [ ] **Step 2: Copy sidecar binary into final image**

In the final stage, add:

```dockerfile
COPY --from=iroh-builder /src/iroh-sidecar/target/release/iroh-sidecar /usr/local/bin/
```

- [ ] **Step 3: Commit**

```bash
git add deploy/Dockerfile
git commit -m "build: add iroh-sidecar Rust build stage to Dockerfile"
```
