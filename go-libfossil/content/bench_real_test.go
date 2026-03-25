package content

import (
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"testing"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/db"
	_ "github.com/dmestas/edgesync/go-libfossil/internal/testdriver"
)

func repoPath() string {
	_, file, _, _ := runtime.Caller(0)
	return filepath.Join(filepath.Dir(file), "..", "..", "testdata", "fossil.fossil")
}

func copyRepo(b *testing.B) string {
	b.Helper()
	src := repoPath()
	if _, err := os.Stat(src); err != nil {
		b.Skipf("fossil repo not found at %s — run: fossil clone https://fossil-scm.org/home testdata/fossil.fossil", src)
	}
	dst := filepath.Join(b.TempDir(), "fossil.fossil")
	in, err := os.Open(src)
	if err != nil {
		b.Fatalf("open source: %v", err)
	}
	defer in.Close()
	out, err := os.Create(dst)
	if err != nil {
		b.Fatalf("create dest: %v", err)
	}
	defer out.Close()
	if _, err := io.Copy(out, in); err != nil {
		b.Fatalf("copy: %v", err)
	}
	return dst
}

func openRepo(b *testing.B) *db.DB {
	b.Helper()
	path := copyRepo(b)
	d, err := db.Open(path)
	if err != nil {
		b.Fatalf("db.Open: %v", err)
	}
	b.Cleanup(func() { d.Close() })
	return d
}

func scanRids(d *db.DB, query string) []libfossil.FslID {
	rows, err := d.Query(query)
	if err != nil {
		return nil
	}
	defer rows.Close()
	var rids []libfossil.FslID
	for rows.Next() {
		var rid int64
		rows.Scan(&rid)
		rids = append(rids, libfossil.FslID(rid))
	}
	return rids
}

// findExpandableAtDepth finds a rid at approximately the given chain depth that
// actually expands without error. Returns 0 if none found.
func findExpandableAtDepth(d *db.DB, targetDepth int) libfossil.FslID {
	rids := scanRids(d, fmt.Sprintf(`
		SELECT rid FROM delta ORDER BY rid LIMIT 500 OFFSET %d
	`, targetDepth*3))

	for _, rid := range rids {
		chain, err := walkDeltaChain(d, rid)
		if err != nil {
			continue
		}
		if len(chain) < 2 {
			continue
		}
		if _, err := Expand(d, rid); err == nil {
			return rid
		}
	}
	return 0
}

// BenchmarkExpandReal_NoDeltas — full text blob, no delta chain.
func BenchmarkExpandReal_NoDeltas(b *testing.B) {
	d := openRepo(b)
	rids := scanRids(d, "SELECT b.rid FROM blob b LEFT JOIN delta d ON b.rid=d.rid WHERE d.rid IS NULL AND b.size > 0 AND b.content IS NOT NULL LIMIT 1")
	if len(rids) == 0 {
		b.Skip("no non-delta blobs found")
	}
	rid := rids[0]
	if _, err := Expand(d, rid); err != nil {
		b.Fatalf("Expand non-delta: %v", err)
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		Expand(d, rid)
	}
}

// BenchmarkExpandReal_ShallowChain — chain depth ~5.
func BenchmarkExpandReal_ShallowChain(b *testing.B) {
	d := openRepo(b)
	rid := findExpandableAtDepth(d, 5)
	if rid == 0 {
		b.Skip("no expandable shallow chain found")
	}
	chain, _ := walkDeltaChain(d, rid)
	b.Logf("rid=%d chain_depth=%d", rid, len(chain))

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		Expand(d, rid)
	}
}

// BenchmarkExpandReal_MidChain — chain depth ~20.
func BenchmarkExpandReal_MidChain(b *testing.B) {
	d := openRepo(b)
	rid := findExpandableAtDepth(d, 20)
	if rid == 0 {
		b.Skip("no expandable mid chain found")
	}
	chain, _ := walkDeltaChain(d, rid)
	b.Logf("rid=%d chain_depth=%d", rid, len(chain))

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		Expand(d, rid)
	}
}

// BenchmarkExpandReal_SharedBase — rids sharing the same base blob.
func BenchmarkExpandReal_SharedBase(b *testing.B) {
	d := openRepo(b)
	// Find the most-shared base and its children.
	rids := scanRids(d, `
		SELECT d.rid FROM delta d
		JOIN (SELECT srcid, count(*) as cnt FROM delta GROUP BY srcid ORDER BY cnt DESC LIMIT 1) top
		ON d.srcid = top.srcid
		LIMIT 5
	`)
	if len(rids) == 0 {
		b.Skip("no shared base rids found")
	}
	// Filter to expandable ones.
	var good []libfossil.FslID
	for _, rid := range rids {
		if _, err := Expand(d, rid); err == nil {
			good = append(good, rid)
		}
	}
	if len(good) == 0 {
		b.Skip("no expandable shared-base rids")
	}
	b.Logf("expanding %d rids sharing a common base", len(good))

	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		Expand(d, good[i%len(good)])
	}
}

// BenchmarkExpandReal_SameRidRepeated — same rid over and over (cache hit scenario).
func BenchmarkExpandReal_SameRidRepeated(b *testing.B) {
	d := openRepo(b)
	rid := findExpandableAtDepth(d, 20)
	if rid == 0 {
		b.Skip("no expandable rid found")
	}
	chain, _ := walkDeltaChain(d, rid)
	b.Logf("rid=%d chain_depth=%d (repeated expansion)", rid, len(chain))

	if _, err := Expand(d, rid); err != nil {
		b.Fatalf("Expand: %v", err)
	}
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		Expand(d, rid)
	}
}

// BenchmarkExpandReal_Scan — expand many delta-stored blobs (repo verify scenario).
func BenchmarkExpandReal_Scan(b *testing.B) {
	d := openRepo(b)
	candidates := scanRids(d, "SELECT rid FROM delta ORDER BY rid LIMIT 2000")

	var rids []libfossil.FslID
	for _, rid := range candidates {
		if _, err := Expand(d, rid); err == nil {
			rids = append(rids, rid)
		}
		if len(rids) >= 500 {
			break
		}
	}
	if len(rids) == 0 {
		b.Fatal("no expandable delta rids")
	}
	b.Logf("expanding %d blobs per iteration", len(rids))

	b.ResetTimer()
	b.ReportMetric(float64(len(rids)), "blobs/op")
	for i := 0; i < b.N; i++ {
		for _, rid := range rids {
			Expand(d, rid)
		}
	}
}

// BenchmarkExpandReal_Profile — mixed workload: shallow + mid + deep.
func BenchmarkExpandReal_Profile(b *testing.B) {
	d := openRepo(b)

	type sample struct {
		rid   libfossil.FslID
		label string
	}
	var samples []sample

	for _, q := range []struct {
		label string
		sql   string
	}{
		{"shallow", "SELECT b.rid FROM blob b LEFT JOIN delta d ON b.rid=d.rid WHERE d.rid IS NULL AND b.size > 0 AND b.content IS NOT NULL LIMIT 10"},
		{"delta", "SELECT rid FROM delta ORDER BY rid LIMIT 20"},
	} {
		for _, rid := range scanRids(d, q.sql) {
			if _, err := Expand(d, rid); err == nil {
				samples = append(samples, sample{rid, q.label})
			}
		}
	}

	b.Logf("workload: %d samples", len(samples))
	b.ResetTimer()
	b.ReportMetric(float64(len(samples)), "blobs/op")
	for i := 0; i < b.N; i++ {
		for _, s := range samples {
			Expand(d, s.rid)
		}
	}
}

// TestExpandReal_ChainDepthDistribution prints repo stats and chain depths.
func TestExpandReal_ChainDepthDistribution(t *testing.T) {
	src := repoPath()
	if _, err := os.Stat(src); err != nil {
		t.Skipf("fossil repo not found at %s", src)
	}
	d, err := db.Open(src)
	if err != nil {
		t.Fatalf("db.Open: %v", err)
	}
	defer d.Close()

	sampleRids := []libfossil.FslID{1534, 1533, 65458, 64392, 30640}
	fmt.Println("=== Chain Depth Samples ===")
	for _, rid := range sampleRids {
		chain, err := walkDeltaChain(d, rid)
		if err != nil {
			t.Logf("rid=%d: error: %v", rid, err)
			continue
		}
		fmt.Printf("rid=%d  chain_depth=%d\n", rid, len(chain))
	}

	var totalBlobs, totalDeltas int
	d.QueryRow("SELECT count(*) FROM blob").Scan(&totalBlobs)
	d.QueryRow("SELECT count(*) FROM delta").Scan(&totalDeltas)
	fmt.Printf("\n=== Repo Stats ===\n")
	fmt.Printf("Total blobs:  %d\n", totalBlobs)
	fmt.Printf("Total deltas: %d (%.1f%%)\n", totalDeltas, float64(totalDeltas)/float64(totalBlobs)*100)

	// Count expandable vs failing.
	candidates := scanRids(d, "SELECT rid FROM delta ORDER BY RANDOM() LIMIT 200")
	var ok, fail int
	for _, rid := range candidates {
		if _, err := Expand(d, rid); err != nil {
			fail++
		} else {
			ok++
		}
	}
	fmt.Printf("\n=== Expand Success Rate (random 200 delta blobs) ===\n")
	fmt.Printf("OK: %d  Failed: %d  Rate: %.1f%%\n", ok, fail, float64(ok)/float64(ok+fail)*100)
}
