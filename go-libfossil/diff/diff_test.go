package diff

import (
	"testing"
)

func TestSplitLines(t *testing.T) {
	tests := []struct {
		name  string
		input string
		want  []string
	}{
		{"empty", "", nil},
		{"single no newline", "hello", []string{"hello"}},
		{"single with newline", "hello\n", []string{"hello"}},
		{"two lines", "a\nb\n", []string{"a", "b"}},
		{"no trailing newline", "a\nb", []string{"a", "b"}},
		{"crlf normalized", "a\r\nb\r\n", []string{"a", "b"}},
		{"mixed eol", "a\nb\r\nc\n", []string{"a", "b", "c"}},
		{"blank lines", "a\n\nb\n", []string{"a", "", "b"}},
	}
	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := splitLines([]byte(tt.input))
			if len(got) != len(tt.want) {
				t.Fatalf("splitLines(%q) = %v (len %d), want %v (len %d)",
					tt.input, got, len(got), tt.want, len(tt.want))
			}
			for i := range got {
				if got[i] != tt.want[i] {
					t.Errorf("line %d: got %q, want %q", i, got[i], tt.want[i])
				}
			}
		})
	}
}

func TestIsBinary(t *testing.T) {
	if isBinary([]byte("hello world")) {
		t.Fatal("text should not be binary")
	}
	if !isBinary([]byte("hello\x00world")) {
		t.Fatal("null byte should be binary")
	}
	if isBinary(nil) {
		t.Fatal("nil should not be binary")
	}
	if isBinary([]byte{}) {
		t.Fatal("empty should not be binary")
	}
}

func TestMyersIdentical(t *testing.T) {
	ops := myers([]string{"a", "b", "c"}, []string{"a", "b", "c"})
	for _, op := range ops {
		if op.kind != opEqual {
			t.Fatalf("identical inputs should produce only opEqual, got %v", op.kind)
		}
	}
	if len(ops) != 3 {
		t.Fatalf("got %d ops, want 3", len(ops))
	}
}

func TestMyersInsert(t *testing.T) {
	ops := myers([]string{"a", "c"}, []string{"a", "b", "c"})
	var inserts int
	for _, op := range ops {
		if op.kind == opInsert {
			inserts++
			if op.text != "b" {
				t.Fatalf("inserted text = %q, want %q", op.text, "b")
			}
		}
	}
	if inserts != 1 {
		t.Fatalf("got %d inserts, want 1", inserts)
	}
}

func TestMyersDelete(t *testing.T) {
	ops := myers([]string{"a", "b", "c"}, []string{"a", "c"})
	var deletes int
	for _, op := range ops {
		if op.kind == opDelete {
			deletes++
			if op.text != "b" {
				t.Fatalf("deleted text = %q, want %q", op.text, "b")
			}
		}
	}
	if deletes != 1 {
		t.Fatalf("got %d deletes, want 1", deletes)
	}
}

func TestMyersEmpty(t *testing.T) {
	ops := myers(nil, []string{"a", "b"})
	var inserts int
	for _, op := range ops {
		if op.kind == opInsert {
			inserts++
		}
	}
	if inserts != 2 {
		t.Fatalf("got %d inserts, want 2", inserts)
	}

	ops = myers([]string{"a", "b"}, nil)
	var deletes int
	for _, op := range ops {
		if op.kind == opDelete {
			deletes++
		}
	}
	if deletes != 2 {
		t.Fatalf("got %d deletes, want 2", deletes)
	}
}

func TestMyersMixed(t *testing.T) {
	src := []string{"a", "b", "c", "d", "e"}
	dst := []string{"a", "x", "c", "e", "f"}
	ops := myers(src, dst)

	// Verify we get a valid edit script: applying ops to src produces dst.
	var result []string
	for _, op := range ops {
		switch op.kind {
		case opEqual, opInsert:
			result = append(result, op.text)
		}
	}
	if len(result) != len(dst) {
		t.Fatalf("applying ops: got %v, want %v", result, dst)
	}
	for i := range result {
		if result[i] != dst[i] {
			t.Fatalf("line %d: got %q, want %q", i, result[i], dst[i])
		}
	}
}
