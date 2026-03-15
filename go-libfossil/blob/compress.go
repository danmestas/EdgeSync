package blob

import (
	"bytes"
	"compress/zlib"
	"fmt"
	"io"

	"github.com/dmestas/edgesync/go-libfossil/simio"
)

func Compress(data []byte) ([]byte, error) {
	var buf bytes.Buffer
	w := zlib.NewWriter(&buf)
	if _, err := w.Write(data); err != nil {
		return nil, fmt.Errorf("zlib compress: %w", err)
	}
	if err := w.Close(); err != nil {
		return nil, fmt.Errorf("zlib close: %w", err)
	}
	return buf.Bytes(), nil
}

func Decompress(data []byte) ([]byte, error) {
	r, err := zlib.NewReader(bytes.NewReader(data))
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
