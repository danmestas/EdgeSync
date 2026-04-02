package agent

import (
	"context"
	"fmt"
	"log/slog"

	"github.com/danmestas/go-libfossil/repo"
	"github.com/danmestas/go-libfossil/sync"
	"github.com/danmestas/go-libfossil/xfer"
	"github.com/nats-io/nats.go"
	"go.opentelemetry.io/otel"
	"go.opentelemetry.io/otel/attribute"
	"go.opentelemetry.io/otel/trace"
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

	tracer := otel.Tracer("edgesync-leaf")

	sub, err := nc.Subscribe(subject, func(msg *nats.Msg) {
		// Extract trace context from incoming NATS headers for cross-service linking.
		reqCtx := extractTraceContext(ctx, msg)

		reqCtx, span := tracer.Start(reqCtx, "nats.handle",
			trace.WithSpanKind(trace.SpanKindServer),
			trace.WithAttributes(
				attribute.String("messaging.system", "nats"),
				attribute.String("messaging.destination.name", subject),
				attribute.Int("messaging.message.body.size", len(msg.Data)),
			),
		)
		defer span.End()

		req, err := xfer.Decode(msg.Data)
		if err != nil {
			span.RecordError(err)
			slog.ErrorContext(reqCtx, "serve-nats: decode error", "error", err)
			return
		}

		resp, err := h(reqCtx, r, req)
		if err != nil {
			span.RecordError(err)
			slog.ErrorContext(reqCtx, "serve-nats: handler error", "error", err)
			resp = &xfer.Message{Cards: []xfer.Card{
				&xfer.ErrorCard{Message: fmt.Sprintf("handler error: %v", err)},
			}}
		}

		respBytes, err := resp.Encode()
		if err != nil {
			span.RecordError(err)
			slog.ErrorContext(reqCtx, "serve-nats: encode error", "error", err)
			return
		}

		if err := msg.Respond(respBytes); err != nil {
			span.RecordError(err)
			slog.ErrorContext(reqCtx, "serve-nats: respond error", "error", err)
		}
		slog.DebugContext(reqCtx, "serve-nats: request handled",
			"request_bytes", len(msg.Data),
			"response_bytes", len(respBytes),
		)
	})
	if err != nil {
		return fmt.Errorf("agent.ServeNATS: subscribe %s: %w", subject, err)
	}

	<-ctx.Done()
	return sub.Unsubscribe()
}
