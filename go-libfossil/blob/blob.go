package blob

import (
	"fmt"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/db"
	"github.com/dmestas/edgesync/go-libfossil/delta"
	"github.com/dmestas/edgesync/go-libfossil/hash"
)

func Store(q db.Querier, content []byte) (libfossil.FslID, string, error) {
	uuid := hash.SHA1(content)

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

	rid, err := result.LastInsertId()
	if err != nil {
		return 0, "", fmt.Errorf("blob.Store lastid: %w", err)
	}

	return libfossil.FslID(rid), uuid, nil
}

func StoreDelta(q db.Querier, content []byte, srcRid libfossil.FslID) (libfossil.FslID, string, error) {
	uuid := hash.SHA1(content)

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

	rid, err := result.LastInsertId()
	if err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta lastid: %w", err)
	}

	_, err = q.Exec("INSERT INTO delta(rid, srcid) VALUES(?, ?)", rid, srcRid)
	if err != nil {
		return 0, "", fmt.Errorf("blob.StoreDelta insert delta: %w", err)
	}

	return libfossil.FslID(rid), uuid, nil
}

func StorePhantom(q db.Querier, uuid string) (libfossil.FslID, error) {
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

	rid, err := result.LastInsertId()
	if err != nil {
		return 0, fmt.Errorf("blob.StorePhantom lastid: %w", err)
	}

	_, err = q.Exec("INSERT INTO phantom(rid) VALUES(?)", rid)
	if err != nil {
		return 0, fmt.Errorf("blob.StorePhantom phantom table: %w", err)
	}

	return libfossil.FslID(rid), nil
}

func Load(q db.Querier, rid libfossil.FslID) ([]byte, error) {
	var content []byte
	var size int64
	err := q.QueryRow("SELECT content, size FROM blob WHERE rid=?", rid).Scan(&content, &size)
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
	// If stored bytes < declared size, content is zlib-compressed.
	if int64(len(content)) == size {
		return content, nil
	}
	return Decompress(content)
}

func Exists(q db.Querier, uuid string) (libfossil.FslID, bool) {
	var rid int64
	err := q.QueryRow("SELECT rid FROM blob WHERE uuid=?", uuid).Scan(&rid)
	if err != nil {
		return 0, false
	}
	return libfossil.FslID(rid), true
}
