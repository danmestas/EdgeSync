//go:build js

package main

import (
	"context"
	"log/slog"
	"syscall/js"
	"time"

	"github.com/dmestas/edgesync/leaf/agent"
	"github.com/dmestas/edgesync/leaf/telemetry"
)

var (
	currentAgent *agent.Agent
	agentCancel  context.CancelFunc
)

func jsNewAgent(_ js.Value, args []js.Value) any {
	if len(args) < 1 {
		return js.ValueOf("error: config object required")
	}
	cfg := args[0]

	repoPath := cfg.Get("repoPath").String()
	natsUrl := cfg.Get("natsUrl").String()

	// Optional fields with defaults
	pollSec := 5
	if p := cfg.Get("pollSeconds"); !p.IsUndefined() && !p.IsNull() {
		pollSec = p.Int()
	}

	// Setup no-op telemetry for browser
	ctx := context.Background()
	telemetry.Setup(ctx, telemetry.TelemetryConfig{ServiceName: "edgesync-browser"})
	obs := telemetry.NewOTelObserver(nil, nil)

	config := agent.Config{
		RepoPath:     repoPath,
		NATSUrl:      natsUrl,
		Push:         true,
		Pull:         true,
		PollInterval: time.Duration(pollSec) * time.Second,
		Observer:     obs,
	}

	var err error
	currentAgent, err = agent.New(config)
	if err != nil {
		return js.ValueOf("error: " + err.Error())
	}
	return js.ValueOf("ok")
}

func jsStart(_ js.Value, _ []js.Value) any {
	if currentAgent == nil {
		return js.ValueOf("error: no agent — call newAgent first")
	}
	if err := currentAgent.Start(); err != nil {
		return js.ValueOf("error: " + err.Error())
	}
	slog.Info("edgesync agent started in browser")
	return js.ValueOf("ok")
}

func jsStop(_ js.Value, _ []js.Value) any {
	if currentAgent != nil {
		if err := currentAgent.Stop(); err != nil {
			return js.ValueOf("error: " + err.Error())
		}
	}
	return js.ValueOf("ok")
}

func jsSyncNow(_ js.Value, _ []js.Value) any {
	if currentAgent != nil {
		currentAgent.SyncNow()
	}
	return js.ValueOf("ok")
}

func main() {
	js.Global().Set("edgesync", js.ValueOf(map[string]any{
		"newAgent": js.FuncOf(jsNewAgent),
		"start":    js.FuncOf(jsStart),
		"stop":     js.FuncOf(jsStop),
		"syncNow":  js.FuncOf(jsSyncNow),
	}))

	slog.Info("edgesync WASM module loaded")

	// Block forever — WASM module stays alive
	select {}
}
