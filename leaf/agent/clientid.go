package agent

import (
	"fmt"
	"io"

	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// ensureClientID returns the persistent edgesync-client-id for this repo,
// generating and storing a new UUIDv4 if none exists.
func ensureClientID(r *repo.Repo, rng io.Reader) (string, error) {
	var id string
	err := r.DB().QueryRow("SELECT value FROM config WHERE name='edgesync-client-id'").Scan(&id)
	if err == nil && id != "" {
		return id, nil
	}

	id, err = generateUUID4(rng)
	if err != nil {
		return "", fmt.Errorf("ensureClientID: %w", err)
	}

	_, err = r.DB().Exec(
		"REPLACE INTO config(name, value, mtime) VALUES('edgesync-client-id', ?, strftime('%s','now'))",
		id,
	)
	if err != nil {
		return "", fmt.Errorf("ensureClientID: store: %w", err)
	}
	return id, nil
}

// generateUUID4 creates a random UUID v4 string from the given reader.
func generateUUID4(rng io.Reader) (string, error) {
	var b [16]byte
	if _, err := rng.Read(b[:]); err != nil {
		return "", err
	}
	b[6] = (b[6] & 0x0f) | 0x40 // version 4
	b[8] = (b[8] & 0x3f) | 0x80 // variant 10
	return fmt.Sprintf("%08x-%04x-%04x-%04x-%012x",
		b[0:4], b[4:6], b[6:8], b[8:10], b[10:16]), nil
}
