// Package repo provides read/write access to Fossil SQLite repository databases.
//
// A Fossil repo is a SQLite database containing content-addressed blobs,
// delta relationships, event metadata, and file mappings. This package
// wraps the schema for use by the leaf agent and bridge.
package repo

import (
	"database/sql"
	"fmt"
)

// Repo represents an open Fossil repository database.
type Repo struct {
	db   *sql.DB
	path string
}

// Open opens a Fossil repository database at the given path.
func Open(path string) (*Repo, error) {
	db, err := sql.Open("sqlite", path)
	if err != nil {
		return nil, fmt.Errorf("repo: open %s: %w", path, err)
	}

	// Verify this looks like a Fossil repo
	var name string
	err = db.QueryRow("SELECT value FROM config WHERE name='project-name'").Scan(&name)
	if err != nil {
		db.Close()
		return nil, fmt.Errorf("repo: %s does not appear to be a Fossil repository: %w", path, err)
	}

	return &Repo{db: db, path: path}, nil
}

// Close closes the repository database.
func (r *Repo) Close() error {
	return r.db.Close()
}

// ProjectCode returns the project-code from the repo config.
func (r *Repo) ProjectCode() (string, error) {
	var code string
	err := r.db.QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&code)
	if err != nil {
		return "", fmt.Errorf("repo: project-code: %w", err)
	}
	return code, nil
}

// Blob represents a content-addressed artifact in the repository.
type Blob struct {
	RID     int
	UUID    string
	Size    int
	Content []byte
}

// GetBlob retrieves an artifact by its UUID (hash).
func (r *Repo) GetBlob(uuid string) (*Blob, error) {
	b := &Blob{UUID: uuid}
	err := r.db.QueryRow(
		"SELECT rid, size, content FROM blob WHERE uuid=?", uuid,
	).Scan(&b.RID, &b.Size, &b.Content)
	if err != nil {
		return nil, fmt.Errorf("repo: get blob %s: %w", uuid, err)
	}
	return b, nil
}

// ListUnclustered returns UUIDs of artifacts that haven't been synced.
// These are artifacts the leaf should advertise via igot messages.
func (r *Repo) ListUnclustered() ([]string, error) {
	rows, err := r.db.Query(
		"SELECT uuid FROM blob WHERE rid NOT IN (SELECT rid FROM unclustered) ORDER BY rid",
	)
	if err != nil {
		// unclustered table may not exist in all repo versions
		return nil, nil
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

// HasBlob checks whether an artifact with the given UUID exists.
func (r *Repo) HasBlob(uuid string) (bool, error) {
	var count int
	err := r.db.QueryRow("SELECT count(*) FROM blob WHERE uuid=?", uuid).Scan(&count)
	if err != nil {
		return false, err
	}
	return count > 0, nil
}
