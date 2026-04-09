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

// SidecarInfo provides the sidecar details that NATSMesh needs for tunnel
// establishment. Passed to Start so the mesh can drive the full sequence.
type SidecarInfo struct {
	EndpointID string // our iroh EndpointId
	SocketPath string // Unix socket for HTTP calls to sidecar
}

// NATSMesh owns the embedded NATS server and tunnel establishment lifecycle.
//
// The caller creates a NATSMesh and calls Start(). Everything else — port
// reservation, tunnel establishment, NATS server configuration — is internal.
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
// To break the chicken-and-egg, Start pre-reserves the leaf port, uses it
// to tell the sidecar where NATS will listen, establishes tunnels to get
// remote ports, then starts NATS with all ports configured.
type NATSMesh struct {
	role      NATSRole
	upstream  string   // optional external NATS URL
	irohPeers []string // remote EndpointIds

	// Set internally during Start.
	reservedLeafPort int
	tunnelPorts      map[string]uint16 // peerID -> local port
	server           *natsserver.Server
	clientURL        string // nats://127.0.0.1:<client-port>
	leafAddr         string // 127.0.0.1:<leaf-port> (for sidecar --nats-addr)
}

// tunnelResponse is the JSON body returned by POST /nats-tunnel/{endpoint-id}.
type tunnelResponse struct {
	Port uint16 `json:"port"`
}

// LeafAddr returns the pre-reserved leaf node listen address for the sidecar's
// --nats-addr. Only valid after ReserveLeafPort or Start. Empty for leaf role.
func (m *NATSMesh) LeafAddr() string {
	return m.leafAddr
}

// ReserveLeafPort pre-allocates an ephemeral TCP port for the NATS leaf node
// listener. The sidecar needs this address at launch (--nats-addr), which
// happens before Start. The listener is closed immediately — NATS rebinds it.
//
// This is the one method that must be called before Start when iroh is enabled.
// For non-iroh usage, Start handles everything.
func (m *NATSMesh) ReserveLeafPort() error {
	if m.role == NATSRoleLeaf {
		return nil
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

// Start brings up the NATS mesh. If sidecar info is provided, it establishes
// tunnels to iroh peers first, then starts the embedded NATS server with
// tunnel ports wired as leaf remotes. Returns the NATS client URL.
//
// Pass nil for sidecar when iroh is not enabled.
func (m *NATSMesh) Start(sidecar *SidecarInfo) (clientURL string, err error) {
	// Step 1: establish tunnels (if sidecar available).
	if sidecar != nil {
		if tunnelErr := m.establishTunnels(sidecar); tunnelErr != nil {
			// Non-fatal: partial connectivity is better than none.
			fmt.Printf("nats mesh: tunnel establishment (non-fatal): %v\n", tunnelErr)
		}
	}

	// Step 2: start embedded NATS with tunnel ports as leaf remotes.
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

	// Determine leaf node address when not pre-reserved.
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

// establishTunnels tells the sidecar to open NATS tunnels to peers.
// Each tunnel returns a local TCP port that NATS will solicit to.
func (m *NATSMesh) establishTunnels(sc *SidecarInfo) error {
	if sc.SocketPath == "" || sc.EndpointID == "" {
		return nil
	}

	client := &http.Client{
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
				return net.Dial("unix", sc.SocketPath)
			},
		},
		Timeout: 10 * time.Second,
	}

	m.tunnelPorts = make(map[string]uint16)

	var errs []error
	for _, peerID := range m.irohPeers {
		if !shouldSolicit(m.role, sc.EndpointID, peerID) {
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
