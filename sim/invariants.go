package sim

import (
	"bytes"
	"fmt"
	"sort"
	"strings"

	libfossil "github.com/danmestas/go-libfossil"
	"github.com/danmestas/go-libfossil/content"
	"github.com/danmestas/go-libfossil/repo"
)

// InvariantResult describes a single invariant check outcome.
type InvariantResult struct {
	Name    string
	Passed  bool
	Details string
}

// CheckBlobConvergence verifies all repos contain the same set of blob UUIDs.
func CheckBlobConvergence(repos []*repo.Repo, labels []string) InvariantResult {
	sets := make([]map[string]bool, len(repos))
	for i, r := range repos {
		sets[i] = make(map[string]bool)
		rows, err := r.DB().Query("SELECT uuid FROM blob WHERE size >= 0")
		if err != nil {
			return InvariantResult{Name: "blob_convergence", Details: fmt.Sprintf("%s: query: %v", labels[i], err)}
		}
		for rows.Next() {
			var uuid string
			rows.Scan(&uuid)
			sets[i][uuid] = true
		}
		rows.Close()
	}

	ref := sets[0]
	var diffs []string
	for i := 1; i < len(sets); i++ {
		for uuid := range ref {
			if !sets[i][uuid] {
				diffs = append(diffs, fmt.Sprintf("%s missing %s", labels[i], uuid))
			}
		}
		for uuid := range sets[i] {
			if !ref[uuid] {
				diffs = append(diffs, fmt.Sprintf("%s has extra %s", labels[i], uuid))
			}
		}
	}

	if len(diffs) == 0 {
		return InvariantResult{Name: "blob_convergence", Passed: true,
			Details: fmt.Sprintf("all %d repos have %d blobs", len(repos), len(ref))}
	}
	sort.Strings(diffs)
	return InvariantResult{Name: "blob_convergence", Details: strings.Join(diffs, "\n")}
}

// CheckContentIntegrity verifies expanded content is byte-identical across repos.
func CheckContentIntegrity(repos []*repo.Repo, labels []string) InvariantResult {
	rows, err := repos[0].DB().Query("SELECT uuid FROM blob WHERE size >= 0")
	if err != nil {
		return InvariantResult{Name: "content_integrity", Details: fmt.Sprintf("query: %v", err)}
	}
	var uuids []string
	for rows.Next() {
		var uuid string
		rows.Scan(&uuid)
		uuids = append(uuids, uuid)
	}
	rows.Close()

	var diffs []string
	for _, uuid := range uuids {
		var refContent []byte
		for i, r := range repos {
			var rid libfossil.FslID
			err := r.DB().QueryRow("SELECT rid FROM blob WHERE uuid=?", uuid).Scan(&rid)
			if err != nil {
				diffs = append(diffs, fmt.Sprintf("%s: %s not found", labels[i], uuid))
				continue
			}
			data, err := content.Expand(r.DB(), rid)
			if err != nil {
				diffs = append(diffs, fmt.Sprintf("%s: expand %s: %v", labels[i], uuid, err))
				continue
			}
			if i == 0 {
				refContent = data
			} else if !bytes.Equal(data, refContent) {
				diffs = append(diffs, fmt.Sprintf("%s: %s content differs (%d vs %d bytes)", labels[i], uuid, len(data), len(refContent)))
			}
		}
	}

	if len(diffs) == 0 {
		return InvariantResult{Name: "content_integrity", Passed: true,
			Details: fmt.Sprintf("all %d blobs match across %d repos", len(uuids), len(repos))}
	}
	sort.Strings(diffs)
	return InvariantResult{Name: "content_integrity", Details: strings.Join(diffs, "\n")}
}

// CheckNoDuplicates verifies no UUID appears more than once in any repo.
func CheckNoDuplicates(repos []*repo.Repo, labels []string) InvariantResult {
	var diffs []string
	for i, r := range repos {
		rows, err := r.DB().Query("SELECT uuid, COUNT(*) as cnt FROM blob GROUP BY uuid HAVING cnt > 1")
		if err != nil {
			diffs = append(diffs, fmt.Sprintf("%s: query: %v", labels[i], err))
			continue
		}
		for rows.Next() {
			var uuid string
			var cnt int
			rows.Scan(&uuid, &cnt)
			diffs = append(diffs, fmt.Sprintf("%s: %s appears %d times", labels[i], uuid, cnt))
		}
		rows.Close()
	}

	if len(diffs) == 0 {
		return InvariantResult{Name: "no_duplicates", Passed: true}
	}
	return InvariantResult{Name: "no_duplicates", Details: strings.Join(diffs, "\n")}
}

// SimReport bundles all context needed to debug a simulation failure.
type SimReport struct {
	Seed          int64
	Severity      Level
	NumLeaves     int
	Schedule      *FaultSchedule
	Invariants    []InvariantResult
	SyncHistories map[string]string
}

func (r *SimReport) Failed() bool {
	for _, inv := range r.Invariants {
		if !inv.Passed {
			return true
		}
	}
	return false
}

func (r *SimReport) String() string {
	var b strings.Builder
	fmt.Fprintf(&b, "Seed: %d\n", r.Seed)
	fmt.Fprintf(&b, "Severity: %s\n", r.Severity)
	fmt.Fprintf(&b, "Leaves: %d\n", r.NumLeaves)
	if r.Schedule != nil && len(r.Schedule.Events) > 0 {
		fmt.Fprintf(&b, "\nFault Schedule:\n%s\n", r.Schedule)
	}
	if len(r.SyncHistories) > 0 {
		fmt.Fprintf(&b, "\nSync Histories:\n")
		for label, hist := range r.SyncHistories {
			fmt.Fprintf(&b, "  %s: %s\n", label, hist)
		}
	}
	fmt.Fprintf(&b, "\nInvariants:\n")
	for _, inv := range r.Invariants {
		status := "PASS"
		if !inv.Passed {
			status = "FAIL"
		}
		fmt.Fprintf(&b, "  %s: %s\n", status, inv.Name)
		if inv.Details != "" {
			fmt.Fprintf(&b, "    %s\n", inv.Details)
		}
	}
	return b.String()
}
