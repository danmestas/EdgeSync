package content

import (
	"fmt"
	"testing"

	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/db"
	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
)

func TestDebugExpandChain(t *testing.T) {
	src := repoPath()
	d, err := db.Open(src)
	if err != nil {
		t.Skipf("db.Open: %v", err)
	}
	defer d.Close()

	chain, err := walkDeltaChain(d, 2)
	if err != nil {
		t.Fatalf("walkDeltaChain: %v", err)
	}
	fmt.Printf("Chain length: %d, root: rid=%d\n", len(chain), chain[0])

	content, err := blob.Load(d, chain[0])
	if err != nil {
		t.Fatalf("Load root rid=%d: %v", chain[0], err)
	}
	fmt.Printf("Root: rid=%d %d bytes\n", chain[0], len(content))

	// Try applying the first delta with CORRECTED interpretation
	// In Fossil: first int = count, '@', second int = offset, ','
	deltaBytes, err := blob.Load(d, chain[1])
	if err != nil {
		t.Fatalf("Load delta: %v", err)
	}
	fmt.Printf("Delta for rid=%d: %d bytes\n", chain[1], len(deltaBytes))
	fmt.Printf("Delta first 100: %q\n", deltaBytes[:min(100, len(deltaBytes))])

	// Manual apply with correct interpretation
	result, err := applyFossilDelta(content, deltaBytes)
	if err != nil {
		t.Fatalf("Manual apply: %v", err)
	}
	fmt.Printf("Result: %d bytes (expected target size from delta header)\n", len(result))
}

// applyFossilDelta applies with correct Fossil format: count@offset,
func applyFossilDelta(source, delta []byte) ([]byte, error) {
	pos := 0
	
	getInt := func() (uint64, error) {
		digits := [128]int{
			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
			-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
			0, 1, 2, 3, 4, 5, 6, 7, 8, 9, -1, -1, -1, -1, -1, -1,
			-1, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24,
			25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, -1, -1, -1, -1, 36,
			-1, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51,
			52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, -1, -1, -1, 63, -1,
		}
		var v uint64
		started := false
		for pos < len(delta) {
			c := delta[pos]
			if c >= 128 || digits[c] < 0 { break }
			v = v*64 + uint64(digits[c])
			pos++
			started = true
		}
		if !started { return 0, fmt.Errorf("no int at pos %d", pos) }
		return v, nil
	}

	targetLen, _ := getInt()
	pos++ // skip \n
	fmt.Printf("Target length: %d\n", targetLen)

	output := make([]byte, 0, targetLen)

	for pos < len(delta) {
		cnt, _ := getInt()
		cmd := delta[pos]
		pos++

		switch cmd {
		case '@':
			// CORRECT: cnt=count, next int=offset
			offset, _ := getInt()
			pos++ // skip ','
			fmt.Printf("  COPY count=%d offset=%d (source_len=%d)\n", cnt, offset, len(source))
			if int(offset+cnt) > len(source) {
				return nil, fmt.Errorf("copy oob: offset=%d cnt=%d srclen=%d", offset, cnt, len(source))
			}
			output = append(output, source[offset:offset+cnt]...)
		case ':':
			fmt.Printf("  INSERT %d bytes\n", cnt)
			output = append(output, delta[pos:pos+int(cnt)]...)
			pos += int(cnt)
		case ';':
			fmt.Printf("  END checksum=%d output=%d target=%d match=%v\n", cnt, len(output), targetLen, uint64(len(output))==targetLen)
			return output, nil
		}
	}
	return output, nil
}
