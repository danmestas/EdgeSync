package telemetry

import (
	"context"
	"log/slog"
)

type agentIDKey struct{}

// WithAgentID stores the given id on the returned context. Readers use
// AgentIDFromContext or rely on CtxHandler's automatic attribute injection.
func WithAgentID(ctx context.Context, id string) context.Context {
	return context.WithValue(ctx, agentIDKey{}, id)
}

// AgentIDFromContext returns the agent id previously stored by WithAgentID,
// or empty string if none.
func AgentIDFromContext(ctx context.Context) string {
	id, _ := ctx.Value(agentIDKey{}).(string)
	return id
}

// CtxHandler wraps a slog.Handler. On every Handle call it reads agent_id
// from ctx (if set) and adds it to the record. Hides context-key plumbing
// from callers; the inner handler is oblivious.
type CtxHandler struct {
	inner slog.Handler
}

// NewCtxHandler wraps inner in a CtxHandler so context-carried agent_id
// attributes surface on every log record without the caller threading
// the field through each call.
func NewCtxHandler(inner slog.Handler) *CtxHandler {
	return &CtxHandler{inner: inner}
}

func (h *CtxHandler) Enabled(ctx context.Context, l slog.Level) bool {
	return h.inner.Enabled(ctx, l)
}

func (h *CtxHandler) Handle(ctx context.Context, r slog.Record) error {
	if id := AgentIDFromContext(ctx); id != "" {
		r.AddAttrs(slog.String("agent_id", id))
	}
	return h.inner.Handle(ctx, r)
}

func (h *CtxHandler) WithAttrs(attrs []slog.Attr) slog.Handler {
	return &CtxHandler{inner: h.inner.WithAttrs(attrs)}
}

func (h *CtxHandler) WithGroup(name string) slog.Handler {
	return &CtxHandler{inner: h.inner.WithGroup(name)}
}
