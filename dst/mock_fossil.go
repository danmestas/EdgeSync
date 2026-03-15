package dst

import (
	"context"
	"fmt"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	libsync "github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// MockFossil simulates a Fossil master server. It manages its own repo
// and implements sync.Transport by handling xfer protocol messages directly.
// Used as the bridge's upstream in deterministic simulation.
type MockFossil struct {
	repo *repo.Repo
}

// Verify interface compliance at compile time.
var _ libsync.Transport = (*MockFossil)(nil)

// NewMockFossil creates a MockFossil backed by the given repo.
func NewMockFossil(r *repo.Repo) *MockFossil {
	return &MockFossil{repo: r}
}

// Repo returns the mock fossil's repository (for seeding artifacts
// and invariant checking).
func (f *MockFossil) Repo() *repo.Repo {
	return f.repo
}

// Exchange handles one xfer request/response round, implementing the
// server side of Fossil's sync protocol.
func (f *MockFossil) Exchange(_ context.Context, req *xfer.Message) (*xfer.Message, error) {
	var (
		clientPush bool
		clientPull bool
		clientHas  = make(map[string]bool) // UUIDs client announced via igot
		resp       []xfer.Card
	)

	// Process incoming cards.
	for _, card := range req.Cards {
		switch c := card.(type) {
		case *xfer.LoginCard:
			// Accept any login.

		case *xfer.PushCard:
			clientPush = true

		case *xfer.PullCard:
			clientPull = true

		case *xfer.CookieCard:
			// Client sent back a cookie — acknowledge.

		case *xfer.PragmaCard:
			// Ignore pragmas.

		case *xfer.IGotCard:
			clientHas[c.UUID] = true
			if clientPush {
				// Client says it has this artifact. If we don't have it, ask for it.
				_, exists := blob.Exists(f.repo.DB(), c.UUID)
				if !exists {
					resp = append(resp, &xfer.GimmeCard{UUID: c.UUID})
				}
			}

		case *xfer.GimmeCard:
			// Client wants this artifact. Send it if we have it.
			if clientPull {
				fileCard, err := f.loadFile(c.UUID)
				if err == nil {
					resp = append(resp, fileCard)
				}
			}

		case *xfer.FileCard:
			// Client is pushing this artifact. Store it.
			if err := f.storeFile(c.UUID, c.DeltaSrc, c.Content); err != nil {
				resp = append(resp, &xfer.ErrorCard{
					Message: fmt.Sprintf("store %s: %v", c.UUID, err),
				})
			}

		case *xfer.CFileCard:
			if err := f.storeFile(c.UUID, c.DeltaSrc, c.Content); err != nil {
				resp = append(resp, &xfer.ErrorCard{
					Message: fmt.Sprintf("store %s: %v", c.UUID, err),
				})
			}
		}
	}

	// If client is pulling, send igot cards for artifacts we have
	// that the client hasn't mentioned.
	if clientPull {
		serverIGots, err := f.buildIGots(clientHas)
		if err != nil {
			return nil, fmt.Errorf("mockfossil: igots: %w", err)
		}
		resp = append(resp, serverIGots...)
	}

	// Always send a cookie.
	resp = append(resp, &xfer.CookieCard{Value: "sim-cookie"})

	return &xfer.Message{Cards: resp}, nil
}

// StoreArtifact adds a raw artifact to the mock fossil's repo.
// Returns the UUID. Used by tests to seed the master with content.
func (f *MockFossil) StoreArtifact(data []byte) (string, error) {
	var uuid string
	err := f.repo.WithTx(func(tx *db.Tx) error {
		rid, u, err := blob.Store(tx, data)
		if err != nil {
			return err
		}
		uuid = u
		_, err = tx.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid)
		return err
	})
	return uuid, err
}

// loadFile loads an artifact by UUID and returns a FileCard.
func (f *MockFossil) loadFile(uuid string) (*xfer.FileCard, error) {
	rid, ok := blob.Exists(f.repo.DB(), uuid)
	if !ok {
		return nil, fmt.Errorf("blob %s not found", uuid)
	}
	data, err := content.Expand(f.repo.DB(), rid)
	if err != nil {
		return nil, err
	}
	return &xfer.FileCard{UUID: uuid, Content: data}, nil
}

// storeFile stores a received artifact, validating its UUID.
func (f *MockFossil) storeFile(uuid, deltaSrc string, payload []byte) error {
	var fullContent []byte
	if deltaSrc != "" {
		// Delta not supported in mock for simplicity — treat as error.
		return fmt.Errorf("mockfossil: delta files not supported (src=%s)", deltaSrc)
	}
	fullContent = payload

	// Verify UUID matches content.
	computed := hash.SHA1(fullContent)
	if computed != uuid {
		// Try SHA3.
		computed = hash.SHA3(fullContent)
		if computed != uuid {
			return fmt.Errorf("UUID mismatch: expected %s, got SHA1=%s SHA3=%s",
				uuid, hash.SHA1(fullContent), hash.SHA3(fullContent))
		}
	}

	return f.repo.WithTx(func(tx *db.Tx) error {
		rid, _, err := blob.Store(tx, fullContent)
		if err != nil {
			return err
		}
		_, err = tx.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid)
		return err
	})
}

// buildIGots returns igot cards for all artifacts in the repo that the
// client hasn't announced.
func (f *MockFossil) buildIGots(clientHas map[string]bool) ([]xfer.Card, error) {
	rows, err := f.repo.DB().Query(
		"SELECT b.uuid FROM blob b WHERE b.size >= 0",
	)
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var cards []xfer.Card
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return nil, err
		}
		if clientHas[uuid] {
			continue
		}
		cards = append(cards, &xfer.IGotCard{UUID: uuid})
	}
	return cards, rows.Err()
}
