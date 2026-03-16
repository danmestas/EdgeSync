package blob

import (
	"bytes"
	"compress/zlib"
	"encoding/binary"
	"fmt"
	"io"

	"github.com/dmestas/edgesync/go-libfossil/simio"
)

// Compress produces Fossil-compatible compressed blob content:
// [4-byte big-endian uncompressed size][zlib-compressed data].
// This matches Fossil's blob_compress() in src/blob.c.
func Compress(data []byte) ([]byte, error) {
	var buf bytes.Buffer
	// 4-byte big-endian uncompressed size prefix.
	binary.Write(&buf, binary.BigEndian, uint32(len(data)))
	w := zlib.NewWriter(&buf)
	if _, err := w.Write(data); err != nil {
		return nil, fmt.Errorf("zlib compress: %w", err)
	}
	if err := w.Close(); err != nil {
		return nil, fmt.Errorf("zlib close: %w", err)
	}
	return buf.Bytes(), nil
}

// Decompress handles Fossil's compressed blob format:
// [4-byte big-endian uncompressed size][zlib-compressed data].
// The 4-byte prefix is skipped before decompressing.
func Decompress(data []byte) ([]byte, error) {
	if len(data) < 5 {
		return nil, fmt.Errorf("zlib decompress: data too short (%d bytes)", len(data))
	}
	// Skip the 4-byte size prefix.
	zlibData := data[4:]
	r, err := zlib.NewReader(bytes.NewReader(zlibData))
	if err != nil {
		return nil, fmt.Errorf("zlib decompress: %w", err)
	}
	defer r.Close()
	out, err := io.ReadAll(r)
	if err != nil {
		return nil, fmt.Errorf("zlib read: %w", err)
	}
	// BUGGIFY: truncate decompressed output to exercise partial-read handling.
	if simio.Buggify(0.02) && len(out) > 1 {
		out = out[:len(out)/2]
	}
	return out, nil
}
