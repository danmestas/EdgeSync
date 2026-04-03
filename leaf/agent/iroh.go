package agent

import (
	"bytes"
	"context"
	"fmt"
	"io"
	"net"
	"net/http"
)

// IrohTransport implements libfossil.Transport over the iroh sidecar's HTTP API.
// It uses the byte-level RoundTrip interface (v0.2.x).
type IrohTransport struct {
	socketPath string
	endpointID string
	client     *http.Client
}

// NewIrohTransport creates a transport that exchanges raw xfer payloads via the
// iroh sidecar HTTP API on the given Unix socket.
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

// RoundTrip sends the raw xfer payload to the sidecar which forwards it
// to the remote peer via iroh, and returns the raw response bytes.
func (t *IrohTransport) RoundTrip(ctx context.Context, payload []byte) ([]byte, error) {
	url := "http://iroh-sidecar/exchange/" + t.endpointID
	req, err := http.NewRequestWithContext(ctx, "POST", url, bytes.NewReader(payload))
	if err != nil {
		return nil, fmt.Errorf("iroh: create request: %w", err)
	}
	req.Header.Set("Content-Type", "application/x-fossil")

	resp, err := t.client.Do(req)
	if err != nil {
		return nil, fmt.Errorf("iroh: sidecar request: %w", err)
	}
	defer resp.Body.Close()

	if resp.StatusCode != http.StatusOK {
		return nil, fmt.Errorf("iroh: sidecar returned %d", resp.StatusCode)
	}

	return io.ReadAll(resp.Body)
}
