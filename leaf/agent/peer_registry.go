package agent

import (
	"fmt"
	"runtime"
	"strings"

	"github.com/danmestas/go-libfossil/repo"
)

var peerRegistryDef = repo.TableDef{
	Columns: []repo.ColumnDef{
		{Name: "peer_id", Type: "text", PK: true},
		{Name: "last_sync", Type: "integer"},
		{Name: "repo_hash", Type: "text"},
		{Name: "version", Type: "text"},
		{Name: "platform", Type: "text"},
		{Name: "capabilities", Type: "text"},
		{Name: "nats_subject", Type: "text"},
		{Name: "addr", Type: "text"},
	},
	Conflict: "self-write",
}

var buildVersion = "dev"

func (a *Agent) ensurePeerRegistry() error {
	if err := repo.EnsureSyncSchema(a.repo.DB()); err != nil {
		return fmt.Errorf("ensurePeerRegistry: %w", err)
	}
	return repo.RegisterSyncedTable(a.repo.DB(), "peer_registry", peerRegistryDef, a.clock.Now().Unix())
}

func (a *Agent) seedPeerRegistry() error {
	row := a.buildPeerRegistryRow()
	return repo.UpsertXRow(a.repo.DB(), "peer_registry", row, a.clock.Now().Unix())
}

func (a *Agent) updatePeerRegistryAfterSync() error {
	row := a.buildPeerRegistryRow()
	return repo.UpsertXRow(a.repo.DB(), "peer_registry", row, a.clock.Now().Unix())
}

func (a *Agent) buildPeerRegistryRow() map[string]any {
	return map[string]any{
		"peer_id":      a.config.PeerID,
		"last_sync":    a.clock.Now().Unix(),
		"repo_hash":    "", // TODO: compute actual repo hash
		"version":      buildVersion,
		"platform":     runtime.GOOS + "/" + runtime.GOARCH,
		"capabilities": strings.Join(a.capabilities(), ","),
		"nats_subject": a.config.SubjectPrefix,
		"addr":         a.config.ServeHTTPAddr,
	}
}

func (a *Agent) capabilities() []string {
	var caps []string
	if a.config.ServeHTTPAddr != "" {
		caps = append(caps, "serve-http")
	}
	if a.config.ServeNATSEnabled {
		caps = append(caps, "serve-nats")
	}
	caps = append(caps, "push", "pull")
	return caps
}
