package telemetry

import (
	"bytes"
	"context"
	"log/slog"
	"strings"
	"testing"
)

func TestCtxHandler_AddsAgentIDFromContext(t *testing.T) {
	var buf bytes.Buffer
	inner := slog.NewTextHandler(&buf, nil)
	h := NewCtxHandler(inner)

	ctx := WithAgentID(context.Background(), "agent-42")
	logger := slog.New(h)
	logger.InfoContext(ctx, "hello")

	if !strings.Contains(buf.String(), "agent_id=agent-42") {
		t.Fatalf("expected agent_id in output, got: %q", buf.String())
	}
}

func TestCtxHandler_NoAgentIDNoAttribute(t *testing.T) {
	var buf bytes.Buffer
	h := NewCtxHandler(slog.NewTextHandler(&buf, nil))

	slog.New(h).InfoContext(context.Background(), "hello")
	if strings.Contains(buf.String(), "agent_id") {
		t.Fatalf("agent_id should be absent, got: %q", buf.String())
	}
}
