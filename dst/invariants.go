package dst

import (
	"fmt"
	"sort"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/repo"
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

// CheckBlobIntegrity verifies that every blob in the repo has a UUID
// matching the hash of its expanded content. This catches corruption
// from buggify (content.Expand byte-flip) or storage bugs.
func CheckBlobIntegrity(nodeID string, r *repo.Repo) error {
	rids, err := allBlobRIDs(r.DB())
	if err != nil {
		return fmt.Errorf("CheckBlobIntegrity(%s): list blobs: %w", nodeID, err)
	}
	for _, rid := range rids {
		if err := content.Verify(r.DB(), rid); err != nil {
			return &InvariantError{
				Invariant: "blob-integrity",
				NodeID:    nodeID,
				Detail:    fmt.Sprintf("rid=%d: %v", rid, err),
			}
		}
	}
	return nil
}

// CheckDeltaChains verifies that every delta's srcid points to an
// existing blob (no dangling references).
func CheckDeltaChains(nodeID string, r *repo.Repo) error {
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
func CheckNoOrphanPhantoms(nodeID string, r *repo.Repo) error {
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
func CheckConvergence(master *repo.Repo, leaves map[NodeID]*repo.Repo) error {
	masterUUIDs, err := allBlobUUIDs(master.DB())
	if err != nil {
		return fmt.Errorf("CheckConvergence: master UUIDs: %w", err)
	}

	for id, leafRepo := range leaves {
		leafUUIDs, err := allBlobUUIDs(leafRepo.DB())
		if err != nil {
			return fmt.Errorf("CheckConvergence: leaf %s UUIDs: %w", id, err)
		}

		// Check master artifacts exist in leaf.
		for uuid := range masterUUIDs {
			if !leafUUIDs[uuid] {
				return &InvariantError{
					Invariant: "convergence",
					NodeID:    string(id),
					Detail:    fmt.Sprintf("missing master artifact %s", uuid),
				}
			}
		}

		// Check leaf artifacts exist in master (for push).
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
// in every leaf. Unlike CheckConvergence, this allows leaves to have
// extra artifacts (useful when only pull is being tested).
func CheckSubsetOf(master *repo.Repo, leaves map[NodeID]*repo.Repo) error {
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

// --- Simulator-level convenience methods ---

// CheckSafety runs all safety invariants on every node in the simulation.
func (s *Simulator) CheckSafety() error {
	for _, id := range s.leafIDs {
		r := s.leaves[id].Repo()
		if err := CheckBlobIntegrity(string(id), r); err != nil {
			return err
		}
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
// Only meaningful after a fault-free sync period.
func (s *Simulator) CheckAllConverged(master *repo.Repo) error {
	leaves := make(map[NodeID]*repo.Repo, len(s.leaves))
	for id, a := range s.leaves {
		leaves[id] = a.Repo()
	}
	return CheckConvergence(master, leaves)
}

// --- Helpers ---

func allBlobRIDs(q db.Querier) ([]libfossil.FslID, error) {
	rows, err := q.Query("SELECT rid FROM blob WHERE size >= 0")
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var rids []libfossil.FslID
	for rows.Next() {
		var rid int64
		if err := rows.Scan(&rid); err != nil {
			return nil, err
		}
		rids = append(rids, libfossil.FslID(rid))
	}
	return rids, rows.Err()
}

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
func AllBlobUUIDsSorted(r *repo.Repo) ([]string, error) {
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
func CountBlobs(r *repo.Repo) (int, error) {
	var count int
	err := r.DB().QueryRow("SELECT count(*) FROM blob WHERE size >= 0").Scan(&count)
	return count, err
}

// HasBlob checks if a specific artifact exists in the repo.
func HasBlob(r *repo.Repo, uuid string) bool {
	_, exists := blob.Exists(r.DB(), uuid)
	return exists
}
