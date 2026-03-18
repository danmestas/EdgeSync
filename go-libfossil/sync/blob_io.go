package sync

import (
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/delta"
	"github.com/dmestas/edgesync/go-libfossil/hash"
	"github.com/dmestas/edgesync/go-libfossil/xfer"
)

// DefaultCloneBatchSize is the number of blobs sent per clone round.
const DefaultCloneBatchSize = 200

// StoreBlob validates, compresses, and stores a received blob.
// Handles both full content and delta-compressed payloads.
// If the blob already exists, it ensures it is in the unclustered table.
func StoreBlob(querier db.Querier, uuid, deltaSrc string, payload []byte) error {
	if uuid == "" {
		panic("sync.StoreBlob: uuid must not be empty")
	}
	if payload == nil {
		panic("sync.StoreBlob: payload must not be nil")
	}
	if !hash.IsValidHash(uuid) {
		return fmt.Errorf("sync: invalid UUID format: %s", uuid)
	}

	fullContent, err := resolvePayload(querier, deltaSrc, payload)
	if err != nil {
		return fmt.Errorf("sync.StoreBlob: %w", err)
	}

	if err := verifyHash(uuid, fullContent); err != nil {
		return err
	}

	return insertBlob(querier, uuid, fullContent)
}

// resolvePayload expands a delta payload against its source, or returns
// the payload as-is for non-delta content.
func resolvePayload(querier db.Querier, deltaSrc string, payload []byte) ([]byte, error) {
	if deltaSrc == "" {
		return payload, nil
	}
	srcRid, ok := blob.Exists(querier, deltaSrc)
	if !ok {
		return nil, fmt.Errorf("delta source %s not found", deltaSrc)
	}
	baseContent, err := content.Expand(querier, srcRid)
	if err != nil {
		return nil, fmt.Errorf("expanding delta source %s: %w", deltaSrc, err)
	}
	applied, err := delta.Apply(baseContent, payload)
	if err != nil {
		return nil, fmt.Errorf("applying delta for %s: %w", deltaSrc, err)
	}
	return applied, nil
}

// verifyHash checks that the content hashes to the expected UUID.
func verifyHash(uuid string, content []byte) error {
	var computed string
	if len(uuid) > 40 {
		computed = hash.SHA3(content)
	} else {
		computed = hash.SHA1(content)
	}
	if computed != uuid {
		return fmt.Errorf("UUID mismatch: expected %s, got %s", uuid, computed)
	}
	return nil
}

// insertBlob compresses and stores content, adding to unclustered.
// If the blob already exists, it just ensures unclustered entry.
func insertBlob(querier db.Querier, uuid string, fullContent []byte) error {
	if rid, ok := blob.Exists(querier, uuid); ok {
		_, err := querier.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid)
		return err
	}
	compressed, err := blob.Compress(fullContent)
	if err != nil {
		return err
	}
	result, err := querier.Exec(
		"INSERT INTO blob(uuid, size, content, rcvid) VALUES(?, ?, ?, 1)",
		uuid, len(fullContent), compressed,
	)
	if err != nil {
		return err
	}
	rid, err := result.LastInsertId()
	if err != nil {
		return err
	}
	_, err = querier.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid)
	return err
}

// LoadBlob loads a blob by UUID and returns it as a FileCard with its size.
// Expands delta chains via content.Expand.
func LoadBlob(querier db.Querier, uuid string) (*xfer.FileCard, int, error) {
	if uuid == "" {
		panic("sync.LoadBlob: uuid must not be empty")
	}
	rid, ok := blob.Exists(querier, uuid)
	if !ok {
		return nil, 0, fmt.Errorf("blob %s not found", uuid)
	}
	data, err := content.Expand(querier, rid)
	if err != nil {
		return nil, 0, fmt.Errorf("expanding blob %s: %w", uuid, err)
	}
	return &xfer.FileCard{UUID: uuid, Content: data}, len(data), nil
}

// ListBlobUUIDs returns all non-phantom blob UUIDs in the repo.
func ListBlobUUIDs(querier db.Querier) ([]string, error) {
	rows, err := querier.Query("SELECT uuid FROM blob WHERE size >= 0")
	if err != nil {
		return nil, err
	}
	defer rows.Close()

	var uuids []string
	for rows.Next() {
		var uuid string
		if err := rows.Scan(&uuid); err != nil {
			return nil, err
		}
		uuids = append(uuids, uuid)
	}
	return uuids, rows.Err()
}

// ListBlobsFromRID returns a paginated batch of blobs for clone.
// Returns file cards, the last rid in the batch, and whether more blobs remain.
func ListBlobsFromRID(querier db.Querier, afterRID, limit int) ([]xfer.FileCard, int, bool, error) {
	if limit <= 0 {
		panic("sync.ListBlobsFromRID: limit must be positive")
	}
	// Fetch limit+1 to detect whether more remain.
	rows, err := querier.Query(
		"SELECT rid, uuid FROM blob WHERE rid > ? AND size >= 0 ORDER BY rid LIMIT ?",
		afterRID, limit+1,
	)
	if err != nil {
		return nil, 0, false, err
	}
	defer rows.Close()

	var cards []xfer.FileCard
	var lastRID int
	for rows.Next() {
		var rid int
		var uuid string
		if err := rows.Scan(&rid, &uuid); err != nil {
			return nil, 0, false, err
		}
		if len(cards) >= limit {
			return cards, lastRID, true, nil
		}
		data, err := content.Expand(querier, libfossil.FslID(rid))
		if err != nil {
			return nil, 0, false, fmt.Errorf("expanding rid %d (%s): %w", rid, uuid, err)
		}
		cards = append(cards, xfer.FileCard{UUID: uuid, Content: data})
		lastRID = rid
	}
	if err := rows.Err(); err != nil {
		return nil, 0, false, err
	}
	return cards, lastRID, false, nil
}
