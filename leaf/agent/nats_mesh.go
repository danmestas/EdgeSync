package agent

import (
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"time"

	natsserver "github.com/nats-io/nats-server/v2/server"
)

// NATSMesh owns the embedded NATS server and tunnel establishment lifecycle.
//
// The startup order matters because the NATS leaf node protocol is asymmetric:
//   - Hub side sends INFO, waits for CONNECT from the leaf.
//   - Leaf side sends CONNECT in response to INFO.
//
// The sidecar's outbound tunnel binds a local TCP listener and returns its
// port. The embedded NATS server must be configured with that port as a leaf
// remote BEFORE starting. Meanwhile, the sidecar's inbound (accepting) side
// needs to know the local NATS leaf port to connect to.
//
// To break the chicken-and-egg between sidecar needing the leaf port and
// NATS needing the tunnel ports, we pre-reserve the leaf port:
//
//  1. ReserveLeafPort — bind an ephemeral port, record it, close the listener
//  2. Start sidecar with --nats-addr pointing to the reserved port
//  3. EstablishTunnels — get outbound tunnel ports from the sidecar
//  4. Start — build NATS opts with the reserved leaf port + tunnel remotes
type NATSMesh struct {
	role      NATSRole
	upstream  string   // optional external NATS URL
	irohPeers []string // remote EndpointIds
	endpointID string  // our iroh EndpointId

	sidecarSocketPath string // for HTTP calls to sidecar

	// reservedLeafPort is pre-allocated so the sidecar can be told the leaf
	// address before the NATS server starts. Zero means "let NATS pick".
	reservedLeafPort int

	// tunnelPorts holds local listener ports returned by the sidecar for each
	// peer we should solicit. Populated by EstablishTunnels, consumed by Start.
	tunnelPorts map[string]uint16 // peerID -> local port

	server    *natsserver.Server
	clientURL string // nats://127.0.0.1:<client-port>
	leafAddr  string // 127.0.0.1:<leaf-port> (for sidecar --nats-addr)
}

// tunnelResponse is the JSON body returned by POST /nats-tunnel/{endpoint-id}.
type tunnelResponse struct {
	Port uint16 `json:"port"`
}

// ReserveLeafPort pre-allocates an ephemeral TCP port for the NATS leaf node
// listener. The port is recorded so buildServerOpts can pass a specific port
// to NATS instead of -1 (random). The listener is closed immediately — NATS
// will rebind it when it starts. There is a small TOCTOU window, but in
// practice collisions are extremely unlikely on localhost.
//
// Call this before starting the sidecar so --nats-addr can be set. Only
// meaningful for hub/peer roles (leaf role has no leaf port).
func (m *NATSMesh) ReserveLeafPort() error {
	if m.role == NATSRoleLeaf {
		return nil // leaf nodes don't listen for leaf connections
	}
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return fmt.Errorf("reserve leaf port: %w", err)
	}
	port := ln.Addr().(*net.TCPAddr).Port
	ln.Close()

	m.reservedLeafPort = port
	m.leafAddr = fmt.Sprintf("127.0.0.1:%d", port)
	return nil
}

// EstablishTunnels tells the sidecar to open NATS tunnels to peers.
// Each tunnel returns a local TCP port that the embedded NATS server will
// solicit to. Must be called BEFORE Start so the ports can be wired into
// the NATS server options.
func (m *NATSMesh) EstablishTunnels() error {
	if m.sidecarSocketPath == "" || m.endpointID == "" {
		return nil // no sidecar or no endpoint — nothing to tunnel
	}

	client := &http.Client{
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
				return net.Dial("unix", m.sidecarSocketPath)
			},
		},
		Timeout: 10 * time.Second,
	}

	m.tunnelPorts = make(map[string]uint16)

	var errs []error
	for _, peerID := range m.irohPeers {
		if !shouldSolicit(m.role, m.endpointID, peerID) {
			continue
		}

		reqURL := fmt.Sprintf("http://iroh-sidecar/nats-tunnel/%s", peerID)
		resp, err := client.Post(reqURL, "", nil)
		if err != nil {
			errs = append(errs, fmt.Errorf("tunnel to %s: %w", peerID, err))
			continue
		}
		if resp.StatusCode != http.StatusOK {
			resp.Body.Close()
			errs = append(errs, fmt.Errorf("tunnel to %s: HTTP %d", peerID, resp.StatusCode))
			continue
		}

		var tr tunnelResponse
		err = json.NewDecoder(resp.Body).Decode(&tr)
		resp.Body.Close()
		if err != nil {
			errs = append(errs, fmt.Errorf("tunnel to %s: decode response: %w", peerID, err))
			continue
		}
		if tr.Port == 0 {
			errs = append(errs, fmt.Errorf("tunnel to %s: sidecar returned port 0", peerID))
			continue
		}
		m.tunnelPorts[peerID] = tr.Port
	}
	return errors.Join(errs...)
}

// Start brings up the embedded NATS server.
// Tunnel ports from EstablishTunnels (if any) are included as leaf remotes.
// Returns the NATS client URL for the agent to connect to.
func (m *NATSMesh) Start() (clientURL string, err error) {
	opts := m.buildServerOpts()
	m.server, err = natsserver.NewServer(opts)
	if err != nil {
		return "", fmt.Errorf("nats mesh: create server: %w", err)
	}
	m.server.Start()
	if !m.server.ReadyForConnections(5 * time.Second) {
		m.server.Shutdown()
		return "", fmt.Errorf("nats mesh: server not ready within 5s (reserved leaf port: %d)", m.reservedLeafPort)
	}

	m.clientURL = m.server.ClientURL()

	// Determine leaf node address for sidecar (when not pre-reserved).
	if m.role != NATSRoleLeaf && m.leafAddr == "" {
		varz, vErr := m.server.Varz(&natsserver.VarzOptions{})
		if vErr == nil && varz.LeafNode.Port > 0 {
			m.leafAddr = fmt.Sprintf("127.0.0.1:%d", varz.LeafNode.Port)
		}
	}

	return m.clientURL, nil
}

// Stop shuts down the embedded NATS server.
func (m *NATSMesh) Stop() {
	if m.server != nil {
		m.server.Shutdown()
		m.server.WaitForShutdown()
		m.server = nil
	}
}

// LeafAddr returns the leaf node listen address for the sidecar's --nats-addr.
func (m *NATSMesh) LeafAddr() string {
	return m.leafAddr
}

// SetEndpointID is called after the sidecar reports its EndpointId.
func (m *NATSMesh) SetEndpointID(id string) {
	m.endpointID = id
}

// SetSidecarSocket provides the sidecar Unix socket path for tunnel HTTP calls.
func (m *NATSMesh) SetSidecarSocket(path string) {
	m.sidecarSocketPath = path
}

// buildServerOpts creates nats-server options based on role.
// Includes tunnel ports as additional leaf remotes so NATS solicits through the
// sidecar's QUIC tunnels.
func (m *NATSMesh) buildServerOpts() *natsserver.Options {
	opts := &natsserver.Options{
		Host:   "127.0.0.1",
		Port:   -1, // random client port
		NoLog:  true,
		NoSigs: true,
	}

	switch m.role {
	case NATSRolePeer, NATSRoleHub:
		if m.reservedLeafPort > 0 {
			opts.LeafNode.Host = "127.0.0.1"
			opts.LeafNode.Port = m.reservedLeafPort
		} else {
			opts.LeafNode.Port = -1 // random leaf node port
		}
	case NATSRoleLeaf:
		// No leaf port — this node solicits outward only.
	}

	// If upstream NATS URL is configured, join as a leaf.
	if m.upstream != "" {
		u, err := url.Parse(m.upstream)
		if err == nil {
			opts.LeafNode.Remotes = append(opts.LeafNode.Remotes,
				&natsserver.RemoteLeafOpts{URLs: []*url.URL{u}},
			)
		}
	}

	// Add tunnel ports as leaf remotes. Each port is a local TCP listener
	// managed by the sidecar, which pipes traffic through QUIC to the remote
	// peer's NATS hub.
	for _, port := range m.tunnelPorts {
		u, err := url.Parse(fmt.Sprintf("nats://127.0.0.1:%d", port))
		if err == nil {
			opts.LeafNode.Remotes = append(opts.LeafNode.Remotes,
				&natsserver.RemoteLeafOpts{URLs: []*url.URL{u}},
			)
		}
	}

	return opts
}

// shouldSolicit determines whether we should initiate a NATS tunnel to a peer.
func shouldSolicit(role NATSRole, myID, peerID string) bool {
	switch role {
	case NATSRoleLeaf:
		return true
	case NATSRoleHub:
		return false
	case NATSRolePeer:
		return myID < peerID
	default:
		return false
	}
}
