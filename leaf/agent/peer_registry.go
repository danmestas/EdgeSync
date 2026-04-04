package agent

import (
	"fmt"
	"runtime"
	"strings"
)

var buildVersion = "dev"

// ensurePeerRegistry creates the peer_registry table if it doesn't exist.
func (a *Agent) ensurePeerRegistry() error {
	_, err := a.repo.DB().Exec(`CREATE TABLE IF NOT EXISTS peer_registry (
		peer_id TEXT PRIMARY KEY,
		last_sync INTEGER,
		repo_hash TEXT,
		version TEXT,
		platform TEXT,
		capabilities TEXT,
		nats_subject TEXT,
		addr TEXT,
		mtime INTEGER DEFAULT 0
	)`)
	if err != nil {
		return fmt.Errorf("ensurePeerRegistry: %w", err)
	}
	return nil
}

func (a *Agent) seedPeerRegistry() error {
	return a.upsertPeerRow()
}

func (a *Agent) updatePeerRegistryAfterSync() error {
	return a.upsertPeerRow()
}

func (a *Agent) upsertPeerRow() error {
	now := a.clock.Now().Unix()
	_, err := a.repo.DB().Exec(`INSERT OR REPLACE INTO peer_registry
		(peer_id, last_sync, repo_hash, version, platform, capabilities, nats_subject, addr, mtime)
		VALUES (?, ?, '', ?, ?, ?, ?, ?, ?)`,
		a.config.PeerID, now, buildVersion,
		runtime.GOOS+"/"+runtime.GOARCH,
		strings.Join(a.capabilities(), ","),
		a.config.SubjectPrefix, a.config.ServeHTTPAddr, now,
	)
	return err
}

func (a *Agent) capabilities() []string {
	var caps []string
	if a.config.ServeHTTPAddr != "" {
		caps = append(caps, "serve-http")
	}
	if a.config.ServeNATSEnabled {
		caps = append(caps, "serve-nats")
	}
	if a.config.IrohEnabled {
		caps = append(caps, "serve-iroh")
	}
	caps = append(caps, "push", "pull")
	return caps
}
