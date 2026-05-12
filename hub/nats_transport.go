package hub

import (
	"context"
	"errors"
	"fmt"
	"time"

	libfossil "github.com/danmestas/libfossil"
	"github.com/nats-io/nats.go"
)

// natsTransport is a libfossil.Transport over NATS request/reply on a
// single subject. RoundTrip publishes the xfer payload as a request,
// waits for the responder, and returns the response bytes.
//
// Used by the hub to pull from peers on `<prefix>.<project-code>.sync`
// after receiving a notification on the sibling `.commit` subject.
type natsTransport struct {
	nc      *nats.Conn
	subject string
	timeout time.Duration
}

const defaultNATSRoundTripTimeout = 30 * time.Second

func newNATSTransport(nc *nats.Conn, subject string, timeout time.Duration) libfossil.Transport {
	if timeout <= 0 {
		timeout = defaultNATSRoundTripTimeout
	}
	return &natsTransport{nc: nc, subject: subject, timeout: timeout}
}

// RoundTrip publishes payload on the configured subject and blocks for
// the reply, honoring ctx. Returns an error if the responder takes
// longer than the transport's timeout (whichever fires first between
// ctx and timeout). An empty reply body is treated as an error — the
// hub-side handler uses an empty body to signal a HandleSync failure,
// and a libfossil Sync round needs a non-empty manifest to make
// progress.
func (t *natsTransport) RoundTrip(ctx context.Context, payload []byte) ([]byte, error) {
	reqCtx, cancel := context.WithTimeout(ctx, t.timeout)
	defer cancel()

	msg, err := t.nc.RequestWithContext(reqCtx, t.subject, payload)
	if err != nil {
		return nil, fmt.Errorf("nats transport: request %s: %w", t.subject, err)
	}
	if len(msg.Data) == 0 {
		return nil, errors.New("nats transport: empty reply (responder signaled HandleSync failure)")
	}
	return msg.Data, nil
}
