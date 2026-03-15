package agent

import (
	"context"
	"fmt"
	"time"

	"github.com/dmestas/edgesync/go-libfossil/xfer"
	"github.com/nats-io/nats.go"
)

// NATSTransport implements sync.Transport over NATS request/reply.
// The subject follows the pattern "fossil.<project-code>.sync".
type NATSTransport struct {
	conn    *nats.Conn
	subject string        // e.g. "fossil.myproj.sync"
	timeout time.Duration // default 30s
}

// NewNATSTransport creates a transport that exchanges xfer messages over
// the given NATS connection. The prefix defaults to "fossil" if empty.
// If timeout is zero, 30 seconds is used.
func NewNATSTransport(conn *nats.Conn, projectCode string, timeout time.Duration, prefix string) *NATSTransport {
	if timeout == 0 {
		timeout = 30 * time.Second
	}
	if prefix == "" {
		prefix = "fossil"
	}
	return &NATSTransport{
		conn:    conn,
		subject: prefix + "." + projectCode + ".sync",
		timeout: timeout,
	}
}

// Exchange encodes the request as zlib-compressed xfer bytes, sends it as
// a NATS request, and decodes the zlib-compressed reply.
func (t *NATSTransport) Exchange(ctx context.Context, req *xfer.Message) (*xfer.Message, error) {
	data, err := req.Encode()
	if err != nil {
		return nil, fmt.Errorf("nats: encode request: %w", err)
	}

	// Use a child context with the transport timeout if the parent has no
	// earlier deadline.
	deadline, ok := ctx.Deadline()
	if !ok || time.Until(deadline) > t.timeout {
		var cancel context.CancelFunc
		ctx, cancel = context.WithTimeout(ctx, t.timeout)
		defer cancel()
	}

	reply, err := t.conn.RequestWithContext(ctx, t.subject, data)
	if err != nil {
		return nil, fmt.Errorf("nats: request on %s: %w", t.subject, err)
	}

	msg, err := xfer.Decode(reply.Data)
	if err != nil {
		return nil, fmt.Errorf("nats: decode reply: %w", err)
	}
	return msg, nil
}
