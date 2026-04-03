package sim

import (
	"fmt"
	"math/rand"

	libfossil "github.com/danmestas/go-libfossil"
)

// SeedResult records what was seeded into a leaf repo.
type SeedResult struct {
	LeafIndex int
	UUIDs     []string
}

// SeedLeaf inserts random checkins into a leaf repo so the sync protocol
// has content to push. Returns the UUIDs of the created artifacts.
func SeedLeaf(r *libfossil.Repo, rng *rand.Rand, count, maxSize int) ([]string, error) {
	var uuids []string

	for i := range count {
		size := rng.Intn(maxSize) + 1
		data := make([]byte, size)
		rng.Read(data)

		_, uuid, err := r.Commit(libfossil.CommitOpts{
			Files: []libfossil.FileToCommit{
				{Name: fmt.Sprintf("seed-%d.bin", i), Content: data},
			},
			Comment: fmt.Sprintf("seed blob %d", i),
			User:    "sim",
		})
		if err != nil {
			return uuids, fmt.Errorf("seed commit %d: %w", i, err)
		}
		uuids = append(uuids, uuid)
	}

	return uuids, nil
}
