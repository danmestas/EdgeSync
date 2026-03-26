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
type IrohTransport struct {
	socketPath string
	endpointID string
	client     *http.Client
}

// NewIrohTransport creates a transport that exchanges xfer messages via the
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
