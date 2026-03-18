package uv

import (
	"github.com/dmestas/edgesync/go-libfossil/db"
)

// Entry represents a row in the unversioned table.
type Entry struct {
	Name  string
	MTime int64  // seconds since 1970
	Hash  string // "" for tombstone (NULL in DB)
	Size  int    // uncompressed size
}

// EnsureSchema creates the unversioned table if it does not exist.
func EnsureSchema(d *db.DB) error {
	panic("not implemented")
}

// Write stores an unversioned file. Hashes content, compresses if beneficial
// (zlib, 80% threshold), and REPLACE INTOs the row. Invalidates uv-hash cache.
func Write(d *db.DB, name string, content []byte, mtime int64) error {
	panic("not implemented")
}

// Delete creates a tombstone for the named file (hash=NULL, sz=0, content=NULL).
// Invalidates uv-hash cache.
func Delete(d *db.DB, name string, mtime int64) error {
	panic("not implemented")
}

// Read returns the decompressed content, mtime, and hash for the named file.
// Returns (nil, 0, "", nil) if the file does not exist.
// Returns (nil, mtime, "", nil) for tombstones.
func Read(d *db.DB, name string) (content []byte, mtime int64, hash string, err error) {
	panic("not implemented")
}

// List returns all entries in the unversioned table, including tombstones.
func List(d *db.DB) ([]Entry, error) {
	panic("not implemented")
}

// ContentHash computes the SHA1 catalog hash over all non-tombstone entries,
// matching Fossil's unversioned_content_hash(). Always SHA1, even in SHA3 repos.
// Caches result in config table as "uv-hash". Returns the cached value if present.
func ContentHash(d *db.DB) (string, error) {
	panic("not implemented")
}

// InvalidateHash removes the cached uv-hash from the config table.
func InvalidateHash(d *db.DB) error {
	panic("not implemented")
}
