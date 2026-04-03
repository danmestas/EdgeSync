package dst

import (
	"fmt"
	"sort"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/db"
)

// InvariantError records which invariant failed, on which node, and why.
type InvariantError struct {
	Invariant string
	NodeID    string // "master" or leaf ID
	Detail    string
}

func (e *InvariantError) Error() string {
	return fmt.Sprintf("invariant %q violated on %s: %s", e.Invariant, e.NodeID, e.Detail)
}

// --- Safety invariants (check anytime) ---

// CheckDeltaChains verifies that every delta's srcid points to an
// existing blob (no dangling references).
func CheckDeltaChains(nodeID string, r *libfossil.Repo) error {
	rows, err := r.DB().Query("SELECT rid, srcid FROM delta")
	if err != nil {
		return fmt.Errorf("CheckDeltaChains(%s): query: %w", nodeID, err)
	}
	defer rows.Close()

	for rows.Next() {
		var rid, srcid int64
		if err := rows.Scan(&rid, &srcid); err != nil {
			return fmt.Errorf("CheckDeltaChains(%s): scan: %w", nodeID, err)
		}
		var exists int
		err := r.DB().QueryRow("SELECT count(*) FROM blob WHERE rid=?", srcid).Scan(&exists)
		if err != nil || exists == 0 {
			return &InvariantError{
				Invariant: "delta-chain",
				NodeID:    nodeID,
				Detail:    fmt.Sprintf("delta rid=%d references srcid=%d which does not exist", rid, srcid),
			}
		}
	}
	return rows.Err()
}

// CheckNoOrphanPhantoms verifies that phantom entries reference blobs
// that actually exist in the blob table (they're just missing content).
func CheckNoOrphanPhantoms(nodeID string, r *libfossil.Repo) error {
	rows, err := r.DB().Query("SELECT p.rid FROM phantom p LEFT JOIN blob b ON p.rid=b.rid WHERE b.rid IS NULL")
	if err != nil {
		return fmt.Errorf("CheckNoOrphanPhantoms(%s): query: %w", nodeID, err)
	}
	defer rows.Close()

	for rows.Next() {
		var rid int64
		rows.Scan(&rid)
		return &InvariantError{
			Invariant: "orphan-phantom",
			NodeID:    nodeID,
			Detail:    fmt.Sprintf("phantom rid=%d has no blob table entry", rid),
		}
	}
	return rows.Err()
}

// --- Convergence invariants (check after fault-free period) ---

// CheckConvergence verifies that every leaf repo contains the same set
// of artifact UUIDs as the master repo.
func CheckConvergence(master *libfossil.Repo, leaves map[NodeID]*libfossil.Repo) error {
	masterUUIDs, err := allBlobUUIDs(master.DB())
	if err != nil {
		return fmt.Errorf("CheckConvergence: master UUIDs: %w", err)
	}

	for id, leafRepo := range leaves {
		leafUUIDs, err := allBlobUUIDs(leafRepo.DB())
		if err != nil {
			return fmt.Errorf("CheckConvergence: leaf %s UUIDs: %w", id, err)
		}

		for uuid := range masterUUIDs {
			if !leafUUIDs[uuid] {
				return &InvariantError{
					Invariant: "convergence",
					NodeID:    string(id),
					Detail:    fmt.Sprintf("missing master artifact %s", uuid),
				}
			}
		}

		for uuid := range leafUUIDs {
			if !masterUUIDs[uuid] {
				return &InvariantError{
					Invariant: "convergence",
					NodeID:    string(id),
					Detail:    fmt.Sprintf("leaf has artifact %s not in master", uuid),
				}
			}
		}
	}
	return nil
}

// CheckSubsetOf verifies that all artifacts in the master are present
// in every leaf.
func CheckSubsetOf(master *libfossil.Repo, leaves map[NodeID]*libfossil.Repo) error {
	masterUUIDs, err := allBlobUUIDs(master.DB())
	if err != nil {
		return fmt.Errorf("CheckSubsetOf: master UUIDs: %w", err)
	}

	for id, leafRepo := range leaves {
		leafUUIDs, err := allBlobUUIDs(leafRepo.DB())
		if err != nil {
			return fmt.Errorf("CheckSubsetOf: leaf %s UUIDs: %w", id, err)
		}
		for uuid := range masterUUIDs {
			if !leafUUIDs[uuid] {
				return &InvariantError{
					Invariant: "subset",
					NodeID:    string(id),
					Detail:    fmt.Sprintf("missing master artifact %s", uuid),
				}
			}
		}
	}
	return nil
}

// --- UV invariants ---

// CheckUVConvergence verifies that all repos have identical UV catalog hashes.
func CheckUVConvergence(master *libfossil.Repo, leaves map[NodeID]*libfossil.Repo) error {
	masterEntries, err := master.UVList()
	if err != nil {
		return fmt.Errorf("CheckUVConvergence: master list: %w", err)
	}
	masterHash := uvCatalogHash(masterEntries)

	for id, leafRepo := range leaves {
		leafEntries, err := leafRepo.UVList()
		if err != nil {
			return fmt.Errorf("CheckUVConvergence: leaf %s list: %w", id, err)
		}
		leafHash := uvCatalogHash(leafEntries)
		if leafHash != masterHash {
			return &InvariantError{
				Invariant: "uv-convergence",
				NodeID:    string(id),
				Detail:    fmt.Sprintf("master=%s leaf=%s", masterHash, leafHash),
			}
		}
	}
	return nil
}

func uvCatalogHash(entries []libfossil.UVEntry) string {
	// Simple catalog comparison: sorted name+hash pairs
	var parts []string
	for _, e := range entries {
		parts = append(parts, fmt.Sprintf("%s:%s", e.Name, e.Hash))
	}
	sort.Strings(parts)
	return fmt.Sprintf("%v", parts)
}

// --- Simulator-level convenience methods ---

// CheckSafety runs all safety invariants on every node in the simulation.
func (s *Simulator) CheckSafety() error {
	for _, id := range s.leafIDs {
		r := s.leaves[id].Repo()
		if err := CheckDeltaChains(string(id), r); err != nil {
			return err
		}
		if err := CheckNoOrphanPhantoms(string(id), r); err != nil {
			return err
		}
	}
	return nil
}

// CheckAllConverged checks convergence between master and all leaves.
func (s *Simulator) CheckAllConverged(master *libfossil.Repo) error {
	leaves := make(map[NodeID]*libfossil.Repo, len(s.leaves))
	for id, a := range s.leaves {
		leaves[id] = a.Repo()
	}
	return CheckConvergence(master, leaves)
}

// CheckAllUVConverged checks UV convergence between master and all leaves.
func (s *Simulator) CheckAllUVConverged(master *libfossil.Repo) error {
	leaves := make(map[NodeID]*libfossil.Repo, len(s.leaves))
	for id, a := range s.leaves {
		leaves[id] = a.Repo()
	}
	return CheckUVConvergence(master, leaves)
}

// --- Helpers ---

func allBlobUUIDs(q db.Querier) (map[string]bool, error) {
	rows, err := q.Query("SELECT uuid FROM blob WHERE size >= 0")
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	uuids := make(map[string]bool)
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return nil, err
		}
		uuids[uuid] = true
	}
	return uuids, rows.Err()
}

// AllBlobUUIDsSorted returns a sorted slice of UUIDs for deterministic comparison.
func AllBlobUUIDsSorted(r *libfossil.Repo) ([]string, error) {
	m, err := allBlobUUIDs(r.DB())
	if err != nil {
		return nil, err
	}
	out := make([]string, 0, len(m))
	for u := range m {
		out = append(out, u)
	}
	sort.Strings(out)
	return out, nil
}

// CountBlobs returns the number of non-phantom blobs in the repo.
func CountBlobs(r *libfossil.Repo) (int, error) {
	var count int
	err := r.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&count)
	return count, err
}

// HasBlob checks if a specific artifact exists in the repo.
func HasBlob(r *libfossil.Repo, uuid string) bool {
	var count int
	err := r.DB().QueryRow("SELECT count(*) FROM blob WHERE uuid=?", uuid).Scan(&count)
	return err == nil && count > 0
}
