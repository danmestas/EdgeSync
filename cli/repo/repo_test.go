package repo_test

import (
	"strings"
	"testing"

	"github.com/alecthomas/kong"
	repocli "github.com/danmestas/EdgeSync/cli/repo"
)

// TestCmdParsesKnownSubcommands proves the re-exported Cmd carries every
// fossil repo subcommand kong sees today. We pick a handful of stable
// subcommands and confirm kong resolves them without erroring on schema.
func TestCmdParsesKnownSubcommands(t *testing.T) {
	type CLI struct {
		repocli.Globals
		Repo repocli.Cmd `cmd:""`
	}

	parser, err := kong.New(&CLI{}, kong.Exit(func(int) {}))
	if err != nil {
		t.Fatalf("kong.New: %v", err)
	}

	subs := []string{"timeline", "ls", "cat", "ci", "co", "diff", "config", "merge"}
	help := captureHelp(t, parser)
	for _, s := range subs {
		if !strings.Contains(help, "repo "+s) {
			t.Errorf("--help output missing 'repo %s' subcommand:\n%s", s, help)
		}
	}
}

// TestGlobalsHaveRepoFlag verifies the Globals re-export still exposes the
// -R / --repo flag downstream consumers depend on.
func TestGlobalsHaveRepoFlag(t *testing.T) {
	type CLI struct {
		repocli.Globals
	}

	parser, err := kong.New(&CLI{}, kong.Exit(func(int) {}))
	if err != nil {
		t.Fatalf("kong.New: %v", err)
	}
	help := captureHelp(t, parser)
	if !strings.Contains(help, "--repo") {
		t.Errorf("--help output missing --repo flag:\n%s", help)
	}
}

func captureHelp(t *testing.T, p *kong.Kong) string {
	t.Helper()
	var sb strings.Builder
	p.Stdout = &sb
	p.Stderr = &sb
	_, _ = p.Parse([]string{"--help"})
	return sb.String()
}
