package dst

import (
	"fmt"
	"sort"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/uv"
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

// --- UV invariants ---

// CheckUVIntegrity verifies that every non-tombstone entry in the
// unversioned table has a hash matching the SHA1 of its decompressed
// content, and sz matches the content length.
func CheckUVIntegrity(nodeID string, r *repo.Repo) error {
	uv.EnsureSchema(r.DB())
	entries, err := uv.List(r.DB())
	if err != nil {
		return fmt.Errorf("CheckUVIntegrity(%s): list: %w", nodeID, err)
	}
	for _, e := range entries {
		if e.Hash == "" {
			continue // tombstone
		}
		content, _, storedHash, err := uv.Read(r.DB(), e.Name)
		if err != nil {
			return &InvariantError{
				Invariant: "uv-integrity",
				NodeID:    nodeID,
				Detail:    fmt.Sprintf("read %q: %v", e.Name, err),
			}
		}
		computed := hash.ContentHash(content, storedHash)
		if computed != storedHash {
			return &InvariantError{
				Invariant: "uv-integrity",
				NodeID:    nodeID,
				Detail:    fmt.Sprintf("file %q: hash mismatch stored=%s computed=%s", e.Name, storedHash, computed),
			}
		}
		if len(content) != e.Size {
			return &InvariantError{
				Invariant: "uv-integrity",
				NodeID:    nodeID,
				Detail:    fmt.Sprintf("file %q: sz=%d but content len=%d", e.Name, e.Size, len(content)),
			}
		}
	}
	return nil
}

// CheckUVConvergence verifies that all repos have identical UV content hashes.
func CheckUVConvergence(master *repo.Repo, leaves map[NodeID]*repo.Repo) error {
	uv.EnsureSchema(master.DB())
	masterHash, err := uv.ContentHash(master.DB())
	if err != nil {
		return fmt.Errorf("CheckUVConvergence: master hash: %w", err)
	}

	for id, leafRepo := range leaves {
		uv.EnsureSchema(leafRepo.DB())
		leafHash, err := uv.ContentHash(leafRepo.DB())
		if err != nil {
			return fmt.Errorf("CheckUVConvergence: leaf %s hash: %w", id, err)
		}
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
		if err := CheckUVIntegrity(string(id), r); err != nil {
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

// CheckAllUVConverged checks UV convergence between master and all leaves.
func (s *Simulator) CheckAllUVConverged(master *repo.Repo) error {
	leaves := make(map[NodeID]*repo.Repo, len(s.leaves))
	for id, a := range s.leaves {
		leaves[id] = a.Repo()
	}
	return CheckUVConvergence(master, leaves)
}

// --- Tag invariants ---

// CheckTagxrefIntegrity verifies that:
// 1. Every tagxref.rid references a valid blob
// 2. Every tagxref.tagid references a valid tag
// 3. Propagated entries (srcid=0) have tagtype=2
// 4. No tagxref.rid references a phantom blob
func CheckTagxrefIntegrity(nodeID string, r *repo.Repo) error {
	// Check all tagxref rows reference valid blobs and tags.
	rows, err := r.DB().Query(`
		SELECT tx.rid, tx.tagid, tx.srcid, tx.tagtype, t.tagname
		FROM tagxref tx
		JOIN tag t ON tx.tagid = t.tagid
	`)
	if err != nil {
		return fmt.Errorf("CheckTagxrefIntegrity(%s): query: %w", nodeID, err)
	}
	defer rows.Close()

	for rows.Next() {
		var rid, tagid, srcid int64
		var tagtype int
		var tagname string
		if err := rows.Scan(&rid, &tagid, &srcid, &tagtype, &tagname); err != nil {
			return fmt.Errorf("CheckTagxrefIntegrity(%s): scan: %w", nodeID, err)
		}

		// Verify rid references a real blob.
		var blobExists int
		if err := r.DB().QueryRow("SELECT count(*) FROM blob WHERE rid=?", rid).Scan(&blobExists); err != nil || blobExists == 0 {
			return &InvariantError{
				Invariant: "tagxref-integrity",
				NodeID:    nodeID,
				Detail:    fmt.Sprintf("tagxref.rid=%d (tag=%s) references non-existent blob", rid, tagname),
			}
		}

		// Verify propagated entries have tagtype=2.
		if srcid == 0 && tagtype != 2 {
			return &InvariantError{
				Invariant: "tagxref-integrity",
				NodeID:    nodeID,
				Detail:    fmt.Sprintf("tagxref.rid=%d (tag=%s) has srcid=0 but tagtype=%d (want 2)", rid, tagname, tagtype),
			}
		}

		// Verify rid is not a phantom.
		var isPhantom int
		r.DB().QueryRow("SELECT count(*) FROM phantom WHERE rid=?", rid).Scan(&isPhantom)
		if isPhantom > 0 {
			return &InvariantError{
				Invariant: "tagxref-integrity",
				NodeID:    nodeID,
				Detail:    fmt.Sprintf("tagxref.rid=%d (tag=%s) references a phantom blob", rid, tagname),
			}
		}
	}
	return rows.Err()
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
