package sim

import (
	"fmt"
	"math/rand"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// SeedResult records what was seeded into a leaf repo.
type SeedResult struct {
	LeafIndex int
	UUIDs     []string
}

// SeedLeaf inserts random blobs into a leaf repo, marking them in
// unclustered and unsent so the sync protocol will push them.
func SeedLeaf(r *repo.Repo, rng *rand.Rand, count, maxSize int) ([]string, error) {
	var uuids []string

	err := r.WithTx(func(tx *db.Tx) error {
		for range count {
			size := rng.Intn(maxSize) + 1
			data := make([]byte, size)
			rng.Read(data)

			rid, uuid, err := blob.Store(tx, data)
			if err != nil {
				return fmt.Errorf("blob.Store: %w", err)
			}

			if _, err := tx.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid); err != nil {
				return fmt.Errorf("insert unclustered: %w", err)
			}
			if _, err := tx.Exec("INSERT OR IGNORE INTO unsent(rid) VALUES(?)", rid); err != nil {
				return fmt.Errorf("insert unsent: %w", err)
			}

			uuids = append(uuids, uuid)
		}
		return nil
	})

	return uuids, err
}
