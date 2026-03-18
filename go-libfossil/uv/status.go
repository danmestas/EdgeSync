package uv

// Status compares a local unversioned file against a remote one and returns
// an action code. Exact port of Fossil's unversioned_status().
//
// localHash="" means no local row (returns 0).
// "-" means deletion marker in either position.
//
// Return codes:
//
//	0 = not present locally (pull)
//	1 = different hash, remote newer or tiebreaker (pull)
//	2 = same hash, remote mtime older (pull mtime only)
//	3 = identical (no action)
//	4 = same hash, remote mtime newer (push mtime only)
//	5 = different hash, local newer or tiebreaker (push)
func Status(localMtime int64, localHash string, remoteMtime int64, remoteHash string) int {
	panic("not implemented")
}
