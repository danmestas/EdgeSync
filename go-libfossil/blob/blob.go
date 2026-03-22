package blob

import (
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/delta"
	"github.com/dmestas/edgesync/go-libfossil/hash"
)

func Store(q db.Querier, content []byte) (rid libfossil.FslID, uuid string, err error) {
	if q == nil {
		panic("blob.Store: q must not be nil")
	}
	if len(content) == 0 {
		panic("blob.Store: content length must be > 0")
	}
	defer func() {
		if err == nil && rid <= 0 {
			panic("blob.Store: postcondition violated: rid <= 0 with no error")
		}
	}()

	uuid = hash.SHA1(content)

	if rid, ok := Exists(q, uuid); ok {
		return rid, uuid, nil
	}

	compressed, err := Compress(content)
	if err != nil {
		return 0, "", fmt.Errorf("blob.Store compress: %w", err)
	}

	result, err := q.Exec(
		"INSERT INTO blob(uuid, size, content, rcvid) VALUES(?, ?, ?, 1)",
		uuid, len(content), compressed,
	)
	if err != nil {
		return 0, "", fmt.Errorf("blob.Store insert: %w", err)
	}

	ridInt, err := result.LastInsertId()
	if err != nil {
		return 0, "", fmt.Errorf("blob.Store lastid: %w", err)
	}

	rid = libfossil.FslID(ridInt)

	// Mark as unclustered — matches Fossil's content_put_ex (content.c:633).
	// Only new blobs reach here; Exists early-return skips this.
	if _, err := q.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid); err != nil {
		return 0, "", fmt.Errorf("blob.Store unclustered: %w", err)
	}

	return rid, uuid, nil
}

func StoreDelta(q db.Querier, content []byte, srcRid libfossil.FslID) (rid libfossil.FslID, uuid string, err error) {
	if q == nil {
		panic("blob.StoreDelta: q must not be nil")
	}
	if len(content) == 0 {
		panic("blob.StoreDelta: content length must be > 0")
	}
	if srcRid <= 0 {
		panic("blob.StoreDelta: srcRid must be > 0")
	}
	defer func() {
		if err == nil && rid <= 0 {
			panic("blob.StoreDelta: postcondition violated: rid <= 0 with no error")
		}
	}()

	uuid = hash.SHA1(content)

	if rid, ok := Exists(q, uuid); ok {
		return rid, uuid, nil
	}

	srcContent, err := Load(q, srcRid)
	if err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta load source: %w", err)
	}

	deltaBytes := delta.Create(srcContent, content)
	compressed, err := Compress(deltaBytes)
	if err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta compress: %w", err)
	}

	result, err := q.Exec(
		"INSERT INTO blob(uuid, size, content, rcvid) VALUES(?, ?, ?, 1)",
		uuid, len(content), compressed,
	)
	if err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta insert blob: %w", err)
	}

	ridInt, err := result.LastInsertId()
	if err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta lastid: %w", err)
	}

	rid = libfossil.FslID(ridInt)
	_, err = q.Exec("INSERT INTO delta(rid, srcid) VALUES(?, ?)", rid, srcRid)
	if err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta insert delta: %w", err)
	}

	// Mark as unclustered — matches Fossil's content_put_ex (content.c:633).
	if _, err := q.Exec("INSERT OR IGNORE INTO unclustered(rid) VALUES(?)", rid); err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta unclustered: %w", err)
	}

	return rid, uuid, nil
}

func StorePhantom(q db.Querier, uuid string) (rid libfossil.FslID, err error) {
	if q == nil {
		panic("blob.StorePhantom: q must not be nil")
	}
	if uuid == "" {
		panic("blob.StorePhantom: uuid must not be empty")
	}
	defer func() {
		if err == nil && rid <= 0 {
			panic("blob.StorePhantom: postcondition violated: rid <= 0 with no error")
		}
	}()

	if rid, ok := Exists(q, uuid); ok {
		return rid, nil
	}

	result, err := q.Exec(
		"INSERT INTO blob(uuid, size, content, rcvid) VALUES(?, -1, NULL, 0)",
		uuid,
	)
	if err != nil {
		return 0, fmt.Errorf("blob.StorePhantom: %w", err)
	}

	ridInt, err := result.LastInsertId()
	if err != nil {
		return 0, fmt.Errorf("blob.StorePhantom lastid: %w", err)
	}

	rid = libfossil.FslID(ridInt)
	_, err = q.Exec("INSERT INTO phantom(rid) VALUES(?)", rid)
	if err != nil {
		return 0, fmt.Errorf("blob.StorePhantom phantom table: %w", err)
	}

	return rid, nil
}

func Load(q db.Querier, rid libfossil.FslID) (result []byte, err error) {
	if q == nil {
		panic("blob.Load: q must not be nil")
	}
	if rid <= 0 {
		panic("blob.Load: rid must be > 0")
	}
	defer func() {
		if err == nil && result == nil {
			panic("blob.Load: postcondition violated: result is nil with no error")
		}
	}()

	var content []byte
	var size int64
	err = q.QueryRow("SELECT content, size FROM blob WHERE rid=?", rid).Scan(&content, &size)
	if err != nil {
		return nil, fmt.Errorf("blob.Load query: %w", err)
	}

	if size == -1 {
		return nil, fmt.Errorf("blob.Load: rid %d is a phantom", rid)
	}

	if content == nil {
		return nil, fmt.Errorf("blob.Load: rid %d has NULL content", rid)
	}

	// Fossil convention: if stored bytes == declared size, content is uncompressed.
	// If stored bytes < declared size, content is zlib-compressed
	// (with 4-byte BE size prefix handled by Decompress).
	if int64(len(content)) == size {
		return content, nil
	}
	return Decompress(content)
}

func Exists(q db.Querier, uuid string) (libfossil.FslID, bool) {
	if q == nil {
		panic("blob.Exists: q must not be nil")
	}
	if uuid == "" {
		panic("blob.Exists: uuid must not be empty")
	}
	var rid int64
	err := q.QueryRow("SELECT rid FROM blob WHERE uuid=?", uuid).Scan(&rid)
	if err != nil {
		return 0, false
	}
	return libfossil.FslID(rid), true
}
