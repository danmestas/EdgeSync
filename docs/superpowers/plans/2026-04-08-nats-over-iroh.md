# NATS-over-iroh Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Tunnel NATS leaf node connections over iroh QUIC streams so P2P agents get presence and messaging without central NATS infrastructure.

**Architecture:** The iroh sidecar gains a TCP-to-QUIC tunnel on a new ALPN. Each Go agent runs an embedded NATS server. A `NATSMesh` module owns the lifecycle: start embedded NATS, start sidecar with `--nats-addr`, establish tunnels to peers based on role and EndpointId comparison. The agent connects its NATS client to the local embedded server.

**Tech Stack:** Rust (iroh-sidecar, tokio, axum), Go (nats-server embedded, leaf agent), iroh 0.97

**Target repos:** EdgeSync (`leaf/agent/`, `iroh-sidecar/src/`)

**Spec:** `docs/superpowers/specs/2026-04-08-nats-over-iroh-design.md`

**IMPORTANT:** Do not add any Claude Code co-author attribution to commits. Work on a feature branch, never push directly to main.

---

## File Structure

### Rust (iroh-sidecar)

| Action | File | Responsibility |
|--------|------|----------------|
| Create | `iroh-sidecar/src/tunnel.rs` | Bidirectional TCP-QUIC pipe for NATS leaf connections |
| Modify | `iroh-sidecar/src/acceptor.rs` | ALPN-based dispatch (xfer vs NATS tunnel) |
| Modify | `iroh-sidecar/src/server.rs` | New endpoint `POST /nats-tunnel/{endpoint_id}` |
| Modify | `iroh-sidecar/src/main.rs` | New CLI arg `--nats-addr`, register second ALPN, mod tunnel |
| Modify | `iroh-sidecar/src/endpoint.rs` | Accept multiple ALPNs |

### Go (leaf agent)

| Action | File | Responsibility |
|--------|------|----------------|
| Create | `leaf/agent/nats_mesh.go` | Embedded NATS + tunnel establishment + startup sequencing |
| Create | `leaf/agent/nats_mesh_test.go` | Unit tests for mesh lifecycle and role logic |
| Modify | `leaf/agent/config.go` | Add `NATSRole`, rename `NATSUrl` → `NATSUpstream` |
| Modify | `leaf/agent/agent.go` | Use NATSMesh in lifecycle, simplify syncTargets |
| Modify | `leaf/agent/sidecar.go` | Accept optional `--nats-addr` arg |

---

### Task 1: Sidecar — TCP tunnel module

**Files:**
- Create: `iroh-sidecar/src/tunnel.rs`

- [ ] **Step 1: Create the tunnel module**

Create `iroh-sidecar/src/tunnel.rs`:

```rust
//! Bidirectional TCP ↔ QUIC tunnel for NATS leaf node connections.
//!
//! When a remote peer connects on ALPN `/edgesync/nats-leaf/1`, we:
//!   1. Accept the bidirectional QUIC stream.
//!   2. Open a TCP connection to the local NATS server.
//!   3. Pipe bytes bidirectionally until either side closes.

use iroh::endpoint::Connection;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

/// Handle an incoming NATS tunnel connection from a remote peer.
///
/// Opens a TCP connection to the local embedded NATS at `nats_addr`,
/// then pipes bytes between the QUIC stream and the TCP socket.
pub async fn handle_connection(connection: Connection, nats_addr: String) {
    let remote_id = connection.remote_id();
    tracing::info!(%remote_id, "nats tunnel: accepted connection");

    loop {
        let (send, recv) = match connection.accept_bi().await {
            Ok(streams) => streams,
            Err(e) => {
                tracing::debug!(%remote_id, "nats tunnel: stream accept ended: {e}");
                break;
            }
        };

        let addr = nats_addr.clone();
        tokio::spawn(async move {
            if let Err(e) = pipe_stream(send, recv, &addr).await {
                tracing::warn!("nats tunnel: pipe error: {e:#}");
            }
        });
    }
}

/// Establish an outbound NATS tunnel to a remote peer.
///
/// Opens a QUIC bidirectional stream on the given connection using the
/// NATS ALPN, then pipes to the local NATS server at `nats_addr`.
pub async fn establish_outbound(connection: Connection, nats_addr: String) -> anyhow::Result<()> {
    let remote_id = connection.remote_id();
    tracing::info!(%remote_id, "nats tunnel: establishing outbound tunnel");

    let (send, recv) = connection.open_bi().await?;
    pipe_stream(send, recv, &nats_addr).await?;

    Ok(())
}

async fn pipe_stream(
    mut quic_send: iroh::endpoint::SendStream,
    mut quic_recv: iroh::endpoint::RecvStream,
    nats_addr: &str,
) -> anyhow::Result<()> {
    let tcp = TcpStream::connect(nats_addr).await?;
    let (mut tcp_read, mut tcp_write) = tcp.into_split();

    // Bidirectional pipe: QUIC → TCP and TCP → QUIC run concurrently.
    let quic_to_tcp = async {
        let mut buf = vec![0u8; 32 * 1024];
        loop {
            let n = match quic_recv.read(&mut buf).await {
                Ok(Some(n)) => n,
                Ok(None) => break,     // QUIC stream finished
                Err(e) => return Err(anyhow::anyhow!("quic read: {e}")),
            };
            tcp_write.write_all(&buf[..n]).await?;
        }
        tcp_write.shutdown().await?;
        Ok::<(), anyhow::Error>(())
    };

    let tcp_to_quic = async {
        let mut buf = vec![0u8; 32 * 1024];
        loop {
            let n = tcp_read.read(&mut buf).await?;
            if n == 0 {
                break; // TCP closed
            }
            quic_send.write_all(&buf[..n]).await?;
        }
        quic_send.finish()?;
        Ok::<(), anyhow::Error>(())
    };

    tokio::select! {
        r = quic_to_tcp => r?,
        r = tcp_to_quic => r?,
    }

    Ok(())
}
```

- [ ] **Step 2: Register the module**

In `iroh-sidecar/src/main.rs`, add at line 1:

```rust
mod acceptor;
mod endpoint;
mod server;
mod tunnel;
```

- [ ] **Step 3: Verify it compiles**

Run: `cd iroh-sidecar && cargo check`

Expected: Compiles (tunnel is defined but not yet called).

- [ ] **Step 4: Commit**

```bash
git add iroh-sidecar/src/tunnel.rs iroh-sidecar/src/main.rs
git commit -m "feat(sidecar): add TCP-QUIC tunnel module for NATS leaf connections"
```

---

### Task 2: Sidecar — ALPN dispatch in acceptor

**Files:**
- Modify: `iroh-sidecar/src/acceptor.rs`
- Modify: `iroh-sidecar/src/endpoint.rs`
- Modify: `iroh-sidecar/src/main.rs`

- [ ] **Step 1: Define ALPN constants in main.rs**

Add constants and pass both ALPNs to the endpoint and acceptor. In `main.rs`, after the `Args` struct:

```rust
pub const XFER_ALPN: &[u8] = b"/edgesync/xfer/1";
pub const NATS_ALPN: &[u8] = b"/edgesync/nats-leaf/1";
```

- [ ] **Step 2: Update endpoint.rs to accept multiple ALPNs**

Change `create_endpoint` to accept a slice of ALPNs:

```rust
pub async fn create_endpoint(
    key_path: &Path,
    alpns: &[&[u8]],
) -> anyhow::Result<(Endpoint, String)> {
    let secret_key = load_or_generate_key(key_path).await?;
    let endpoint_id = secret_key.public().to_string();

    let endpoint = Endpoint::builder(presets::N0)
        .secret_key(secret_key)
        .alpns(alpns.iter().map(|a| a.to_vec()).collect())
        .bind()
        .await?;

    tracing::info!(%endpoint_id, "iroh endpoint bound");
    Ok((endpoint, endpoint_id))
}
```

- [ ] **Step 3: Update main.rs to register both ALPNs and pass nats_addr**

Add `--nats-addr` CLI arg:

```rust
#[derive(Parser)]
#[command(name = "iroh-sidecar", about = "Iroh P2P proxy for EdgeSync")]
struct Args {
    #[arg(long)]
    socket: PathBuf,

    #[arg(long)]
    key_path: PathBuf,

    #[arg(long)]
    callback: String,

    #[arg(long, default_value = "/edgesync/xfer/1")]
    alpn: String,

    /// Local NATS address to tunnel leaf node connections to.
    /// If omitted, NATS tunneling is disabled.
    #[arg(long)]
    nats_addr: Option<String>,
}
```

Update the endpoint creation call:

```rust
let alpns: Vec<&[u8]> = if args.nats_addr.is_some() {
    vec![args.alpn.as_bytes(), NATS_ALPN]
} else {
    vec![args.alpn.as_bytes()]
};

let (ep, endpoint_id) = endpoint::create_endpoint(&args.key_path, &alpns).await?;
```

Pass `nats_addr` to the accept loop:

```rust
let nats_addr = args.nats_addr.clone();
let accept_handle = tokio::spawn(async move {
    acceptor::run_accept_loop(accept_ep, callback_url, nats_addr).await;
});
```

- [ ] **Step 4: Update acceptor.rs to dispatch by ALPN**

Change `run_accept_loop` signature to accept `nats_addr`:

```rust
pub async fn run_accept_loop(
    endpoint: Endpoint,
    callback_url: String,
    nats_addr: Option<String>,
) {
```

In `handle_incoming`, dispatch based on ALPN. The connection's ALPN is available via `connection.alpn()`:

```rust
async fn handle_incoming(
    incoming: iroh::endpoint::Incoming,
    client: reqwest::Client,
    callback_url: String,
    nats_addr: Option<String>,
) -> anyhow::Result<()> {
    let connection = incoming.accept()?.await?;
    let remote_id = connection.remote_id();
    let alpn = connection.alpn();
    tracing::info!(%remote_id, alpn = %String::from_utf8_lossy(&alpn), "accepted incoming connection");

    if alpn == crate::NATS_ALPN {
        if let Some(addr) = nats_addr {
            crate::tunnel::handle_connection(connection, addr).await;
        } else {
            tracing::warn!(%remote_id, "NATS tunnel connection received but --nats-addr not configured");
        }
        return Ok(());
    }

    // Existing xfer handling (unchanged from here down)
    let mut stream_tasks = JoinSet::new();
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
            if let Err(e) = handle_stream(&mut send, &mut recv, &client, &callback_url, remote_id).await {
                tracing::warn!(%remote_id, "stream handler error: {e:#}");
            }
        });
    }
    while let Some(result) = stream_tasks.join_next().await {
        if let Err(e) = result {
            tracing::warn!(%remote_id, "stream task panicked: {e:#}");
        }
    }
    Ok(())
}
```

Update the `tasks.spawn` call in `run_accept_loop` to pass `nats_addr`:

```rust
tasks.spawn(async move {
    if let Err(e) = handle_incoming(incoming, client, callback_url, nats_addr).await {
        tracing::warn!("accept loop: error handling incoming connection: {e:#}");
    }
});
```

(Note: `nats_addr` needs to be cloned per iteration since it's moved into the spawned task.)

- [ ] **Step 5: Verify it compiles**

Run: `cd iroh-sidecar && cargo check`

Expected: Compiles. Existing behavior unchanged when `--nats-addr` is not provided.

- [ ] **Step 6: Commit**

```bash
git add iroh-sidecar/src/
git commit -m "feat(sidecar): ALPN dispatch — route xfer and NATS tunnel connections separately"
```

---

### Task 3: Sidecar — outbound tunnel HTTP endpoint

**Files:**
- Modify: `iroh-sidecar/src/server.rs`

- [ ] **Step 1: Add nats_addr to AppState**

In `server.rs`, add a field to `AppState`:

```rust
pub struct AppState {
    pub endpoint: Endpoint,
    pub endpoint_id: String,
    pub alpn: Vec<u8>,
    pub nats_addr: Option<String>,  // NEW
    pub conn_cache: Arc<Mutex<HashMap<String, Connection>>>,
    pub shutdown_tx: Arc<Mutex<Option<oneshot::Sender<()>>>>,
}
```

Update the constructor:

```rust
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
```

- [ ] **Step 2: Add the route and handler**

Add route to `build_router`:

```rust
pub fn build_router(state: AppState) -> Router {
    Router::new()
        .route("/exchange/{endpoint_id}", post(handle_exchange))
        .route("/nats-tunnel/{endpoint_id}", post(handle_nats_tunnel))
        .route("/status", get(handle_status))
        .route("/shutdown", post(handle_shutdown))
        .with_state(state)
}
```

Add the handler:

```rust
/// POST /nats-tunnel/{endpoint-id}
///
/// Establish a long-lived NATS tunnel to a remote peer. Opens a QUIC
/// connection on the NATS ALPN and pipes bytes to the local NATS server.
/// Returns 200 immediately; the tunnel runs in a background task.
async fn handle_nats_tunnel(
    State(state): State<AppState>,
    Path(remote_id_str): Path<String>,
) -> Result<Response, AppError> {
    let nats_addr = state.nats_addr.as_ref()
        .ok_or_else(|| AppError::bad_request("NATS tunneling not configured (no --nats-addr)"))?
        .clone();

    let remote_id = EndpointId::from_str(&remote_id_str)
        .map_err(|e| AppError::bad_request(format!("invalid endpoint id: {e}")))?;

    // Connect to remote peer on the NATS ALPN.
    let nats_alpn = crate::NATS_ALPN.to_vec();
    let addr: EndpointAddr = remote_id.into();
    let conn = state.endpoint.connect(addr, &nats_alpn)
        .await
        .map_err(AppError::internal)?;

    tracing::info!(remote = %remote_id_str, "nats tunnel: established outbound connection");

    // Spawn the tunnel in the background — it runs until either side closes.
    tokio::spawn(async move {
        if let Err(e) = crate::tunnel::establish_outbound(conn, nats_addr).await {
            tracing::warn!(remote = %remote_id_str, "nats tunnel: outbound ended: {e:#}");
        }
    });

    Ok(StatusCode::OK.into_response())
}
```

- [ ] **Step 3: Update main.rs to pass nats_addr to AppState**

In `main.rs`:

```rust
let state = server::AppState::new(
    ep.clone(),
    endpoint_id,
    args.alpn.as_bytes().to_vec(),
    args.nats_addr.clone(),
    shutdown_tx,
);
```

- [ ] **Step 4: Verify it compiles**

Run: `cd iroh-sidecar && cargo check`

- [ ] **Step 5: Build the sidecar**

Run: `cd iroh-sidecar && cargo build`

Expected: Successful build.

- [ ] **Step 6: Commit**

```bash
git add iroh-sidecar/src/
git commit -m "feat(sidecar): add POST /nats-tunnel endpoint for outbound NATS tunnels"
```

---

### Task 4: Go — Config changes

**Files:**
- Modify: `leaf/agent/config.go`

- [ ] **Step 1: Add NATSRole and rename NATSUrl**

In `config.go`, add the `NATSRole` type and field, rename `NATSUrl`:

```go
// NATSRole determines how the embedded NATS server participates in the mesh.
type NATSRole string

const (
	// NATSRolePeer is the default. Accepts and solicits leaf connections
	// based on EndpointId comparison (lower ID solicits).
	NATSRolePeer NATSRole = "peer"

	// NATSRoleHub only accepts leaf connections, never solicits.
	// Use for dedicated servers or cluster nodes.
	NATSRoleHub NATSRole = "hub"

	// NATSRoleLeaf always solicits outward, never accepts.
	// Use for WASM browsers or lightweight clients.
	NATSRoleLeaf NATSRole = "leaf"
)
```

In the `Config` struct, rename `NATSUrl` to `NATSUpstream` and add `NATSRole`:

```go
	// NATSRole determines mesh topology (default "peer").
	NATSRole NATSRole

	// NATSUpstream is an optional external NATS server URL.
	// If set, the embedded NATS joins it as a leaf node.
	// Replaces the old NATSUrl field.
	NATSUpstream string
```

Remove or comment the old `NATSUrl` field. Update `applyDefaults()`:

```go
func (c *Config) applyDefaults() {
	// NATSUrl default removed — embedded NATS replaces it.
	if c.NATSRole == "" {
		c.NATSRole = NATSRolePeer
	}
	// ... rest unchanged ...
}
```

Update `validate()` to check NATSRole:

```go
func (c *Config) validate() error {
	if c.RepoPath == "" {
		return errors.New("agent: config: RepoPath is required")
	}
	switch c.NATSRole {
	case NATSRolePeer, NATSRoleHub, NATSRoleLeaf:
		// valid
	default:
		return fmt.Errorf("agent: config: invalid NATSRole %q (must be peer, hub, or leaf)", c.NATSRole)
	}
	return nil
}
```

- [ ] **Step 2: Fix all references to NATSUrl**

Search for `NATSUrl` across the leaf module and update to `NATSUpstream`. Key files: `agent.go`, `nats.go`, `cmd/leaf/main.go`.

- [ ] **Step 3: Verify it compiles**

Run: `cd leaf && go build ./...`

- [ ] **Step 4: Commit**

```bash
git add leaf/
git commit -m "feat(agent): add NATSRole config, rename NATSUrl to NATSUpstream"
```

---

### Task 5: Go — NATSMesh module

**Files:**
- Create: `leaf/agent/nats_mesh.go`
- Create: `leaf/agent/nats_mesh_test.go`

- [ ] **Step 1: Write the test**

Create `leaf/agent/nats_mesh_test.go`:

```go
package agent

import (
	"testing"
	"time"

	natsserver "github.com/nats-io/nats-server/v2/server"
	"github.com/nats-io/nats.go"
)

func TestNATSMeshStartStop(t *testing.T) {
	mesh := &NATSMesh{
		role: NATSRolePeer,
	}
	clientURL, err := mesh.Start()
	if err != nil {
		t.Fatalf("Start: %v", err)
	}
	defer mesh.Stop()

	if clientURL == "" {
		t.Fatal("clientURL is empty")
	}

	// Verify we can connect and publish.
	nc, err := nats.Connect(clientURL, nats.Timeout(2*time.Second))
	if err != nil {
		t.Fatalf("nats.Connect: %v", err)
	}
	defer nc.Close()

	if err := nc.Publish("test.subject", []byte("hello")); err != nil {
		t.Fatalf("Publish: %v", err)
	}
	nc.Flush()
	t.Logf("mesh started at %s, publish OK", clientURL)
}

func TestNATSMeshRoleLeafPort(t *testing.T) {
	// peer/hub should have a leaf node port; leaf should not.
	for _, role := range []NATSRole{NATSRolePeer, NATSRoleHub, NATSRoleLeaf} {
		t.Run(string(role), func(t *testing.T) {
			mesh := &NATSMesh{role: role}
			opts := mesh.buildServerOpts()

			if role == NATSRoleLeaf {
				if opts.LeafNode.Port != 0 {
					t.Errorf("leaf role should not set LeafNode.Port, got %d", opts.LeafNode.Port)
				}
			} else {
				if opts.LeafNode.Port != -1 {
					t.Errorf("%s role should set LeafNode.Port to -1 (random), got %d", role, opts.LeafNode.Port)
				}
			}
		})
	}
}

func TestTunnelShouldSolicit(t *testing.T) {
	tests := []struct {
		role     NATSRole
		myID     string
		peerID   string
		expected bool
	}{
		{NATSRoleLeaf, "zzz", "aaa", true},   // leaf always solicits
		{NATSRoleHub, "aaa", "zzz", false},    // hub never solicits
		{NATSRolePeer, "aaa", "zzz", true},    // lower ID solicits
		{NATSRolePeer, "zzz", "aaa", false},   // higher ID waits
		{NATSRolePeer, "aaa", "aaa", false},   // same ID (shouldn't happen) — don't solicit
	}
	for _, tt := range tests {
		name := string(tt.role) + "_" + tt.myID + "_vs_" + tt.peerID
		t.Run(name, func(t *testing.T) {
			got := shouldSolicit(tt.role, tt.myID, tt.peerID)
			if got != tt.expected {
				t.Errorf("shouldSolicit(%s, %s, %s) = %v, want %v",
					tt.role, tt.myID, tt.peerID, got, tt.expected)
			}
		})
	}
}
```

- [ ] **Step 2: Write the implementation**

Create `leaf/agent/nats_mesh.go`:

```go
package agent

import (
	"fmt"
	"net/http"
	"time"

	natsserver "github.com/nats-io/nats-server/v2/server"
)

// NATSMesh owns the embedded NATS server and tunnel establishment lifecycle.
// It encapsulates startup ordering: embedded NATS → sidecar → tunnels.
type NATSMesh struct {
	role         NATSRole
	upstream     string    // optional external NATS URL
	irohPeers    []string  // remote EndpointIds
	endpointID   string    // our iroh EndpointId (set after sidecar starts)
	sidecar      *sidecar  // nil if iroh disabled

	server       *natsserver.Server
	clientURL    string    // nats://127.0.0.1:<client-port>
	leafAddr     string    // 127.0.0.1:<leaf-port> (for sidecar --nats-addr)
}

// Start brings up the embedded NATS server and, if a sidecar is configured,
// starts it with --nats-addr and establishes tunnels to peers.
// Returns the NATS client URL for the agent to connect to.
func (m *NATSMesh) Start() (clientURL string, err error) {
	// 1. Start embedded NATS.
	opts := m.buildServerOpts()
	m.server, err = natsserver.NewServer(opts)
	if err != nil {
		return "", fmt.Errorf("nats mesh: create server: %w", err)
	}
	m.server.Start()
	if !m.server.ReadyForConnections(5 * time.Second) {
		m.server.Shutdown()
		return "", fmt.Errorf("nats mesh: server not ready within 5s")
	}

	m.clientURL = m.server.ClientURL()
	m.leafAddr = fmt.Sprintf("127.0.0.1:%d", opts.LeafNode.Port)

	// 2. Start sidecar with --nats-addr (if iroh enabled).
	if m.sidecar != nil && m.leafAddr != "" && m.role != NATSRoleLeaf {
		// For peer/hub, sidecar needs the leaf node port for incoming tunnels.
		// Sidecar startup is handled by the agent — we just record the addr.
	}

	// 3. Establish tunnels (if sidecar is ready and peers are known).
	if m.sidecar != nil && m.endpointID != "" {
		if err := m.establishTunnels(); err != nil {
			// Non-fatal — tunnels can be retried.
			// Log but don't fail startup.
			fmt.Printf("nats mesh: tunnel establishment: %v\n", err)
		}
	}

	return m.clientURL, nil
}

// Stop shuts down tunnels, sidecar, and embedded NATS.
func (m *NATSMesh) Stop() {
	if m.server != nil {
		m.server.Shutdown()
		m.server.WaitForShutdown()
		m.server = nil
	}
}

// LeafAddr returns the leaf node listen address for the sidecar's --nats-addr.
// Empty if role is "leaf" (no leaf port).
func (m *NATSMesh) LeafAddr() string {
	return m.leafAddr
}

// SetEndpointID is called after the sidecar starts and reports its EndpointId.
func (m *NATSMesh) SetEndpointID(id string) {
	m.endpointID = id
}

// SetSidecar provides the sidecar reference for tunnel establishment.
func (m *NATSMesh) SetSidecar(s *sidecar) {
	m.sidecar = s
}

// buildServerOpts creates nats-server options based on role.
func (m *NATSMesh) buildServerOpts() *natsserver.Options {
	opts := &natsserver.Options{
		Host:   "127.0.0.1",
		Port:   -1, // random client port
		NoLog:  true,
		NoSigs: true,
	}

	switch m.role {
	case NATSRolePeer, NATSRoleHub:
		opts.LeafNode.Port = -1 // random leaf node port
	case NATSRoleLeaf:
		// No leaf port — this node solicits outward only.
	}

	// If upstream NATS URL is configured, join as a leaf.
	if m.upstream != "" {
		opts.LeafNode.Remotes = []*natsserver.RemoteLeafOpts{
			{URLs: []*natsserver.URL{{URL: m.upstream}}},
		}
	}

	return opts
}

// establishTunnels tells the sidecar to open NATS tunnels to peers
// based on role and EndpointId comparison.
func (m *NATSMesh) establishTunnels() error {
	if m.sidecar == nil {
		return nil
	}

	client := &http.Client{
		Transport: &http.Transport{
			DialContext: sidecarDialer(m.sidecar.socketPath),
		},
		Timeout: 10 * time.Second,
	}

	for _, peerID := range m.irohPeers {
		if !shouldSolicit(m.role, m.endpointID, peerID) {
			continue
		}

		url := fmt.Sprintf("http://iroh-sidecar/nats-tunnel/%s", peerID)
		resp, err := client.Post(url, "", nil)
		if err != nil {
			return fmt.Errorf("tunnel to %s: %w", peerID[:12], err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("tunnel to %s: HTTP %d", peerID[:12], resp.StatusCode)
		}
	}
	return nil
}

// shouldSolicit determines whether we should initiate a NATS tunnel to a peer.
func shouldSolicit(role NATSRole, myID, peerID string) bool {
	switch role {
	case NATSRoleLeaf:
		return true // always solicit outward
	case NATSRoleHub:
		return false // never solicit
	case NATSRolePeer:
		return myID < peerID // lower ID solicits
	default:
		return false
	}
}
```

Note: `sidecarDialer` is a helper that returns a `DialContext` func for the Unix socket. Check if this already exists in `sidecar.go` — if so, extract and reuse. If not, define it:

```go
func sidecarDialer(socketPath string) func(ctx context.Context, _, _ string) (net.Conn, error) {
	return func(ctx context.Context, _, _ string) (net.Conn, error) {
		return net.Dial("unix", socketPath)
	}
}
```

- [ ] **Step 3: Run tests**

Run: `cd leaf && go test ./agent/ -run TestNATSMesh -v -count=1`
Run: `cd leaf && go test ./agent/ -run TestTunnelShouldSolicit -v -count=1`

Expected: All PASS.

- [ ] **Step 4: Commit**

```bash
git add leaf/agent/nats_mesh.go leaf/agent/nats_mesh_test.go
git commit -m "feat(agent): add NATSMesh module — embedded NATS + tunnel establishment"
```

---

### Task 6: Go — Wire NATSMesh into agent lifecycle

**Files:**
- Modify: `leaf/agent/agent.go`
- Modify: `leaf/agent/sidecar.go`

- [ ] **Step 1: Add mesh field to Agent**

In `agent.go`, add the mesh to the Agent struct and update `Start()`:

The agent's `Start()` should:
1. Create NATSMesh with config
2. Call `mesh.Start()` to get clientURL
3. Connect NATS client to clientURL (instead of `cfg.NATSUpstream`)
4. If iroh enabled: start sidecar with `--nats-addr mesh.LeafAddr()`
5. After sidecar ready: `mesh.SetEndpointID(status.EndpointID)` + `mesh.SetSidecar(sidecar)` + `mesh.establishTunnels()`
6. Continue with ServeNATS, HTTP, poll loop as before

- [ ] **Step 2: Update sidecar.go to accept natsAddr**

Add `natsAddr` field to the `sidecar` struct:

```go
type sidecar struct {
	binPath     string
	socketPath  string
	keyPath     string
	callbackURL string
	alpn        string
	natsAddr    string // NEW: optional, for --nats-addr
	cmd         *exec.Cmd
	exited      chan struct{}
	exitErr     error
}
```

In `spawn()`, conditionally add the flag:

```go
args := []string{
	"--socket", s.socketPath,
	"--key-path", s.keyPath,
	"--callback", s.callbackURL,
	"--alpn", s.alpn,
}
if s.natsAddr != "" {
	args = append(args, "--nats-addr", s.natsAddr)
}
s.cmd = exec.Command(s.binPath, args...)
```

- [ ] **Step 3: Update syncTargets in agent.go**

When NATSMesh is active, the agent should use a single NATSTransport connected to the mesh's clientURL instead of separate NATS + iroh targets. The iroh peers are now reached via NATS leaf node routing through the tunnel.

Keep `IrohTransport` targets as a fallback for backwards compat with peers that don't have embedded NATS.

- [ ] **Step 4: Run agent tests**

Run: `cd leaf && go test ./agent/ -v -count=1 -timeout=60s`

Expected: All existing tests PASS. New mesh tests PASS.

- [ ] **Step 5: Commit**

```bash
git add leaf/agent/
git commit -m "feat(agent): wire NATSMesh into agent lifecycle"
```

---

### Task 7: Integration test — P2P sync over NATS-over-iroh

**Files:**
- Create: `sim/nats_iroh_test.go`

- [ ] **Step 1: Write the integration test**

This test requires two iroh sidecars. It follows the pattern of `sim/iroh_test.go`:

```go
func TestNATSOverIrohSync(t *testing.T) {
	if testing.Short() {
		t.Skip("skipping NATS-over-iroh integration test in short mode")
	}
	findIrohSidecar(t) // skip if binary not found

	dir := t.TempDir()
	projCode := "abcdef0123456789abcdef0123456789abcdef01"
	srvCode := "fedcba9876543210fedcba9876543210fedcba98"

	// 1. Create two repos.
	// 2. Seed blobs into repo 0.
	// 3. Start two agents with NATSRole=peer, IrohEnabled=true, no NATSUpstream.
	//    Each agent uses NATSMesh (embedded NATS + sidecar).
	// 4. Exchange EndpointIds, configure peers, establish tunnels.
	// 5. Trigger sync on both agents.
	// 6. Verify convergence: repo 1 has all blobs from repo 0.
}
```

The full test will closely mirror `TestIrohConvergence` but with NATS-over-iroh tunnels instead of direct IrohTransport.

- [ ] **Step 2: Run the test**

Run: `cd sim && go test -run TestNATSOverIrohSync -v -count=1 -timeout=120s`

Expected: PASS — blobs sync from agent 0 to agent 1 via embedded NATS over iroh tunnels.

- [ ] **Step 3: Commit**

```bash
git add sim/nats_iroh_test.go
git commit -m "test: integration test for P2P sync over NATS-over-iroh (EDG-59)"
```

---

### Task 8: Full test suite verification

**Files:** None (verification only)

- [ ] **Step 1: Build sidecar**

Run: `cd iroh-sidecar && cargo build`

Expected: Successful.

- [ ] **Step 2: Run leaf agent tests**

Run: `cd leaf && go test ./... -v -count=1 -timeout=60s`

Expected: All PASS.

- [ ] **Step 3: Run sim tests (non-short)**

Run: `go test ./sim/ -v -count=1 -timeout=120s`

Expected: All PASS (iroh tests skip if sidecar binary not present).

- [ ] **Step 4: Run EdgeSync full suite**

Run: `make test`

Expected: PASS.
