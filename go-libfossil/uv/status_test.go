package uv

import "testing"

func TestStatus(t *testing.T) {
	tests := []struct {
		name        string
		localMtime  int64
		localHash   string
		remoteMtime int64
		remoteHash  string
		want        int
	}{
		{"no-local-row", 0, "", 100, "abc123", 0},
		{"no-local-row-remote-deleted", 0, "", 100, "-", 0},
		{"remote-newer-diff-hash", 100, "aaa", 200, "bbb", 1},
		{"same-mtime-local-hash-less", 100, "aaa", 100, "bbb", 1},
		{"remote-deletion-newer", 100, "abc123", 200, "-", 1},
		{"same-hash-remote-older", 200, "abc123", 100, "abc123", 4},
		{"identical", 100, "abc123", 100, "abc123", 3},
		{"identical-deletion", 100, "-", 100, "-", 3},
		{"same-hash-remote-newer", 100, "abc123", 200, "abc123", 2},
		{"local-newer-diff-hash", 200, "bbb", 100, "aaa", 5},
		{"same-mtime-local-hash-greater", 100, "bbb", 100, "aaa", 5},
		{"local-deletion-newer", 200, "-", 100, "abc123", 5},
		{"same-mtime-local-deletion-tiebreaker", 100, "-", 100, "abc123", 5},
	}

	for _, tt := range tests {
		t.Run(tt.name, func(t *testing.T) {
			got := Status(tt.localMtime, tt.localHash, tt.remoteMtime, tt.remoteHash)
			if got != tt.want {
				t.Errorf("Status(%d, %q, %d, %q) = %d, want %d",
					tt.localMtime, tt.localHash, tt.remoteMtime, tt.remoteHash, got, tt.want)
			}
		})
	}
}
