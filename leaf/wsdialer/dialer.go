//go:build js

package wsdialer

import (
	"fmt"
	"net"
)

// WSDialer implements nats.CustomDialer using the browser's WebSocket API.
// The URL field is the ws:// address of the NATS server's WebSocket port.
//
// Usage:
//
//	nats.Connect(url, nats.SetCustomDialer(&wsdialer.WSDialer{URL: "ws://localhost:8080"}))
type WSDialer struct {
	URL string
}

// Dial creates a new WebSocket connection. The network and address parameters
// are ignored — the dialer always connects to the configured URL.
func (d *WSDialer) Dial(network, address string) (net.Conn, error) {
	if d.URL == "" {
		panic("wsdialer: URL must not be empty")
	}
	conn := newWSConn(d.URL)
	if err := conn.waitOpen(); err != nil {
		return nil, fmt.Errorf("wsdialer: %w", err)
	}
	return conn, nil
}
