package notify

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"encoding/json"
	"fmt"
	"io"

	libfossil "github.com/danmestas/go-libfossil"
)

// InitNotifyRepo creates a new notify.fossil repo at the given path.
// Returns the opened *libfossil.Repo — caller owns it and must Close() it.
func InitNotifyRepo(path string) (*libfossil.Repo, error) {
	return libfossil.Create(path, libfossil.CreateOpts{
		User: "notify",
	})
}

// CommitMessage serializes a message to JSON and commits it to the repo.
func CommitMessage(r *libfossil.Repo, msg Message) error {
	data, err := json.MarshalIndent(msg, "", "  ")
	if err != nil {
		return fmt.Errorf("marshal message: %w", err)
	}

	_, _, err = r.Commit(libfossil.CommitOpts{
		Files: []libfossil.FileToCommit{
			{Name: msg.FilePath(), Content: data},
		},
		Comment: "notify: " + msg.ID,
		User:    "notify",
	})
	if err != nil {
		return fmt.Errorf("commit message: %w", err)
	}
	return nil
}

// ReadMessage reads and deserializes a message from the repo by its file path.
// It queries the repo database for the file content across all checkins.
func ReadMessage(r *libfossil.Repo, filePath string) (Message, error) {
	data, err := readFileContent(r, filePath)
	if err != nil {
		return Message{}, err
	}

	var msg Message
	if err := json.Unmarshal(data, &msg); err != nil {
		return Message{}, fmt.Errorf("unmarshal message: %w", err)
	}
	return msg, nil
}

// readFileContent reads the latest content of a file by name from the repo database.
// It joins mlink -> blob to find the file content for the most recent checkin.
func readFileContent(r *libfossil.Repo, name string) ([]byte, error) {
	var content []byte
	err := r.DB().QueryRow(`
		SELECT b.content
		FROM mlink ml
		JOIN filename fn ON fn.fnid = ml.fnid
		JOIN blob b ON b.rid = ml.fid
		WHERE fn.name = ?
		ORDER BY ml.mid DESC
		LIMIT 1
	`, name).Scan(&content)
	if err != nil {
		return nil, fmt.Errorf("read file %q: %w", name, err)
	}

	// Fossil blob format: [4-byte BE uncompressed size][zlib data].
	decoded, err := decompressBlob(content)
	if err != nil {
		return nil, fmt.Errorf("decompress file %q: %w", name, err)
	}
	return decoded, nil
}

// decompressBlob decodes Fossil's blob format: 4-byte big-endian size prefix + zlib payload.
func decompressBlob(data []byte) ([]byte, error) {
	if len(data) < 4 {
		return data, nil // Too small to be compressed.
	}
	uncompSize := binary.BigEndian.Uint32(data[:4])
	zr, err := zlib.NewReader(bytes.NewReader(data[4:]))
	if err != nil {
		return nil, fmt.Errorf("zlib init: %w", err)
	}
	defer zr.Close()

	out := make([]byte, uncompSize)
	if _, err := io.ReadFull(zr, out); err != nil {
		return nil, fmt.Errorf("zlib read: %w", err)
	}
	return out, nil
}
