package agent

import (
	"context"
	"fmt"
	"log/slog"

	"github.com/danmestas/go-libfossil/repo"
	"github.com/danmestas/go-libfossil/sync"
	"github.com/danmestas/go-libfossil/xfer"
	"github.com/nats-io/nats.go"
)

// ServeNATS subscribes to the given subject and dispatches incoming
// xfer requests to the handler. Blocks until ctx is cancelled.
func ServeNATS(ctx context.Context, nc *nats.Conn, subject string, r *repo.Repo, h sync.HandleFunc) error {
	if nc == nil {
		panic("agent.ServeNATS: nc must not be nil")
	}
	if subject == "" {
		panic("agent.ServeNATS: subject must not be empty")
	}
	if r == nil {
		panic("agent.ServeNATS: r must not be nil")
	}
	if h == nil {
		panic("agent.ServeNATS: h must not be nil")
	}

	sub, err := nc.Subscribe(subject, func(msg *nats.Msg) {
		req, err := xfer.Decode(msg.Data)
		if err != nil {
			slog.ErrorContext(ctx, "serve-nats: decode error", "error", err)
			return
		}

		resp, err := h(ctx, r, req)
		if err != nil {
			slog.ErrorContext(ctx, "serve-nats: handler error", "error", err)
			resp = &xfer.Message{Cards: []xfer.Card{
				&xfer.ErrorCard{Message: fmt.Sprintf("handler error: %v", err)},
			}}
		}

		respBytes, err := resp.Encode()
		if err != nil {
			slog.ErrorContext(ctx, "serve-nats: encode error", "error", err)
			return
		}

		if err := msg.Respond(respBytes); err != nil {
			slog.ErrorContext(ctx, "serve-nats: respond error", "error", err)
		}
	})
	if err != nil {
		return fmt.Errorf("agent.ServeNATS: subscribe %s: %w", subject, err)
	}

	<-ctx.Done()
	return sub.Unsubscribe()
}
