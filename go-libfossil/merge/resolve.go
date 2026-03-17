package merge

import (
	"bufio"
	"path/filepath"
	"strings"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/blob"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// PatternRule maps a glob pattern to a strategy name.
type PatternRule struct {
	Glob     string
	Strategy string
}

// Resolver picks the merge strategy for a given filename.
type Resolver struct {
	patterns []PatternRule
	fallback string
}

// LoadResolver reads the .edgesync-merge file from the repo at the given
// version, plus the merge-strategy config key as fallback.
func LoadResolver(r *repo.Repo, tipRid libfossil.FslID) *Resolver {
	res := &Resolver{fallback: "three-way"}

	var cfgDefault string
	if r.DB().QueryRow("SELECT value FROM config WHERE name='merge-strategy'").Scan(&cfgDefault) == nil && cfgDefault != "" {
		res.fallback = cfgDefault
	}

	if tipRid > 0 {
		files, err := manifest.ListFiles(r, tipRid)
		if err == nil {
			for _, f := range files {
				if f.Name == ".edgesync-merge" {
					rid, ok := blob.Exists(r.DB(), f.UUID)
					if ok {
						data, err := content.Expand(r.DB(), rid)
						if err == nil {
							res.patterns = parseMergeFile(data)
						}
					}
					break
				}
			}
		}
	}

	return res
}

// Resolve returns the strategy name for a filename.
func (res *Resolver) Resolve(filename string) string {
	for _, p := range res.patterns {
		matched, _ := filepath.Match(p.Glob, filepath.Base(filename))
		if matched {
			return p.Strategy
		}
	}
	return res.fallback
}

func parseMergeFile(data []byte) []PatternRule {
	var rules []PatternRule
	scanner := bufio.NewScanner(strings.NewReader(string(data)))
	for scanner.Scan() {
		line := strings.TrimSpace(scanner.Text())
		if line == "" || strings.HasPrefix(line, "#") {
			continue
		}
		parts := strings.Fields(line)
		if len(parts) >= 2 {
			rules = append(rules, PatternRule{Glob: parts[0], Strategy: parts[1]})
		}
	}
	return rules
}
