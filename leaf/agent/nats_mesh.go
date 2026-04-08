package agent

import (
	"context"
	"fmt"
	"net"
	"net/http"
	"net/url"
	"time"

	natsserver "github.com/nats-io/nats-server/v2/server"
)

// NATSMesh owns the embedded NATS server and tunnel establishment lifecycle.
type NATSMesh struct {
	role      NATSRole
	upstream  string   // optional external NATS URL
	irohPeers []string // remote EndpointIds
	endpointID string  // our iroh EndpointId

	sidecarSocketPath string // for HTTP calls to sidecar

	server    *natsserver.Server
	clientURL string // nats://127.0.0.1:<client-port>
	leafAddr  string // 127.0.0.1:<leaf-port> (for sidecar --nats-addr)
}

// Start brings up the embedded NATS server.
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
		return "", fmt.Errorf("nats mesh: server not ready within 5s")
	}

	m.clientURL = m.server.ClientURL()

	// Determine leaf node address for sidecar.
	if m.role != NATSRoleLeaf {
		varz, vErr := m.server.Varz(&natsserver.VarzOptions{})
		if vErr == nil && varz.LeafNode.Port > 0 {
			m.leafAddr = fmt.Sprintf("127.0.0.1:%d", varz.LeafNode.Port)
		}
	}

	return m.clientURL, nil
}

// EstablishTunnels tells the sidecar to open NATS tunnels to peers.
// Call after sidecar is started and endpointID is known.
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

	for _, peerID := range m.irohPeers {
		if !shouldSolicit(m.role, m.endpointID, peerID) {
			continue
		}

		reqURL := fmt.Sprintf("http://iroh-sidecar/nats-tunnel/%s", peerID)
		resp, err := client.Post(reqURL, "", nil)
		if err != nil {
			return fmt.Errorf("tunnel to %s: %w", peerID, err)
		}
		resp.Body.Close()
		if resp.StatusCode != http.StatusOK {
			return fmt.Errorf("tunnel to %s: HTTP %d", peerID, resp.StatusCode)
		}
	}
	return nil
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
		u, err := url.Parse(m.upstream)
		if err == nil {
			opts.LeafNode.Remotes = []*natsserver.RemoteLeafOpts{
				{URLs: []*url.URL{u}},
			}
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
