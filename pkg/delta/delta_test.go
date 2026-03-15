package delta

import (
	"testing"
)

func TestReaderGetInt(t *testing.T) {
	// Fossil base-64: '0'=0, '9'=9, 'A'=10, 'Z'=35, '_'=36, 'a'=37, 'z'=61, '~'=63
	tests := []struct {
		input string
		want  uint64
	}{
		{"0", 0},
		{"1", 1},
		{"9", 9},
		{"A", 10},
		{"Z", 35},
		{"10", 64},
	}

	for _, tt := range tests {
		r := &reader{data: []byte(tt.input)}
		got, err := r.getInt()
		if err != nil {
			t.Errorf("getInt(%q): unexpected error: %v", tt.input, err)
			continue
		}
		if got != tt.want {
			t.Errorf("getInt(%q) = %d, want %d", tt.input, got, tt.want)
		}
	}
}

func TestApplyInsertOnly(t *testing.T) {
	// A delta that just inserts "hello" with no source dependency.
	// We'll generate real test vectors from fossil once the codec is verified.
	// For now, test the reader/parser mechanics.
	r := &reader{data: []byte("5\n")}
	targetLen, err := r.getInt()
	if err != nil {
		t.Fatal(err)
	}
	if targetLen != 5 {
		t.Fatalf("targetLen = %d, want 5", targetLen)
	}
}
