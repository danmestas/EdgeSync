package content

import (
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/deck"
)

const (
	// ClusterThreshold is the minimum number of unclustered, non-phantom blobs
	// before cluster generation triggers.
	ClusterThreshold = 100

	// ClusterMaxSize is the maximum number of M-cards per cluster artifact.
	ClusterMaxSize = 800
)

// GenerateClusters creates cluster artifacts for unclustered, non-phantom blobs.
// Returns the number of clusters created.
func GenerateClusters(q db.Querier) (int, error) {
	if q == nil {
		panic("content.GenerateClusters: q must not be nil")
	}

	// Count non-phantom unclustered entries.
	var count int
	err := q.QueryRow(`
		SELECT count(*) FROM unclustered u
		WHERE NOT EXISTS (SELECT 1 FROM phantom WHERE rid = u.rid)
	`).Scan(&count)
	if err != nil {
		return 0, fmt.Errorf("content.GenerateClusters count: %w", err)
	}
	if count < ClusterThreshold {
		return 0, nil
	}

	// Query UUIDs sorted.
	rows, err := q.Query(`
		SELECT b.uuid FROM unclustered u
		JOIN blob b ON b.rid = u.rid
		WHERE NOT EXISTS (SELECT 1 FROM phantom WHERE rid = u.rid)
		ORDER BY b.uuid
	`)
	if err != nil {
		return 0, fmt.Errorf("content.GenerateClusters query: %w", err)
	}
	defer rows.Close()

	var uuids []string
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return 0, fmt.Errorf("content.GenerateClusters scan: %w", err)
		}
		uuids = append(uuids, uuid)
	}
	if err := rows.Err(); err != nil {
		return 0, fmt.Errorf("content.GenerateClusters rows: %w", err)
	}

	// Batch into clusters.
	var clusterRIDs []libfossil.FslID
	for len(uuids) > 0 {
		batchSize := ClusterMaxSize
		if batchSize > len(uuids) {
			batchSize = len(uuids)
		}
		// Only split if more than ClusterThreshold remain after this batch.
		remaining := len(uuids) - batchSize
		if remaining > 0 && remaining < ClusterThreshold {
			batchSize = len(uuids) // take all
		}

		batch := uuids[:batchSize]
		uuids = uuids[batchSize:]

		// Build cluster artifact.
		d := &deck.Deck{Type: deck.Cluster, M: batch}
		data, err := d.Marshal()
		if err != nil {
			return len(clusterRIDs), fmt.Errorf("content.GenerateClusters marshal: %w", err)
		}

		rid, _, err := blob.Store(q, data)
		if err != nil {
			return len(clusterRIDs), fmt.Errorf("content.GenerateClusters store: %w", err)
		}

		// Apply cluster singleton tag (tagid=7, tagtype=1).
		// This is the same logic as manifest.CrosslinkCluster but inlined to
		// avoid an import cycle (content -> manifest -> content).
		// We skip the M-card UUID resolution because all UUIDs are known to
		// exist — we just queried them from the blob table.
		if _, err := q.Exec(
			"INSERT OR REPLACE INTO tagxref(tagid, tagtype, srcid, origid, value, mtime, rid) VALUES(7, 1, ?, ?, NULL, 0, ?)",
			rid, rid, rid,
		); err != nil {
			return len(clusterRIDs), fmt.Errorf("content.GenerateClusters tag: %w", err)
		}

		clusterRIDs = append(clusterRIDs, rid)
	}

	// Clean up unclustered: remove all non-phantom entries except cluster RIDs.
	// Cluster blobs themselves stay in unclustered until a future clustering pass.
	if len(clusterRIDs) > 0 {
		placeholders := ""
		args := make([]any, len(clusterRIDs))
		for i, rid := range clusterRIDs {
			if i > 0 {
				placeholders += ","
			}
			placeholders += "?"
			args[i] = rid
		}
		query := fmt.Sprintf(
			"DELETE FROM unclustered WHERE rid NOT IN (%s) AND NOT EXISTS (SELECT 1 FROM phantom WHERE rid = unclustered.rid)",
			placeholders,
		)
		if _, err := q.Exec(query, args...); err != nil {
			return len(clusterRIDs), fmt.Errorf("content.GenerateClusters cleanup: %w", err)
		}
	}

	return len(clusterRIDs), nil
}
