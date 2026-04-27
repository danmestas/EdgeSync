package cli

import (
	"fmt"
	"net"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"
	"time"

	libfossilcli "github.com/danmestas/libfossil/cli"
)

type DoctorCmd struct {
	NATSUrl string `help:"NATS server URL to check connectivity" default:"nats://localhost:4222"`
}

func (c *DoctorCmd) Run(g *libfossilcli.Globals) error {
	passed, warned, failed := 0, 0, 0

	check := func(name string, fn func() (string, error)) {
		detail, err := fn()
		if err != nil {
			fmt.Printf("  FAIL  %s: %s\n", name, err)
			failed++
		} else if strings.HasPrefix(detail, "WARN:") {
			fmt.Printf("  WARN  %s: %s\n", name, strings.TrimPrefix(detail, "WARN:"))
			warned++
		} else {
			fmt.Printf("  OK    %s: %s\n", name, detail)
			passed++
		}
	}

	fmt.Println("=== edgesync doctor ===")
	fmt.Println()

	// Go runtime
	check("go version", func() (string, error) {
		return runtime.Version(), nil
	})

	// go build works with -buildvcs=false
	check("go build (buildvcs)", func() (string, error) {
		cmd := exec.Command("go", "version")
		out, err := cmd.Output()
		if err != nil {
			return "", fmt.Errorf("go not found in PATH")
		}
		return strings.TrimSpace(string(out)), nil
	})

	// fossil binary
	check("fossil binary", func() (string, error) {
		path, err := exec.LookPath("fossil")
		if err != nil {
			return "WARN:" + "not found in PATH (needed for sim/interop tests)", nil
		}
		out, _ := exec.Command(path, "version").Output()
		return strings.TrimSpace(string(out)), nil
	})

	// Repository path
	check("repository", func() (string, error) {
		if g.Repo != "" {
			if _, err := os.Stat(g.Repo); err != nil {
				return "", fmt.Errorf("%s: %w", g.Repo, err)
			}
			return g.Repo, nil
		}
		r, err := g.OpenRepo()
		if err != nil {
			return "WARN:" + "no .fossil file found in cwd or parents (use -R <path>)", nil
		}
		r.Close()
		return "auto-detected", nil
	})

	// NATS connectivity
	check("nats connectivity", func() (string, error) {
		u, err := url.Parse(c.NATSUrl)
		if err != nil {
			return "", fmt.Errorf("invalid URL: %s", c.NATSUrl)
		}
		host := u.Host
		if !strings.Contains(host, ":") {
			host += ":4222"
		}
		conn, err := net.DialTimeout("tcp", host, 2*time.Second)
		if err != nil {
			return "WARN:" + fmt.Sprintf("%s unreachable (%v)", c.NATSUrl, err), nil
		}
		conn.Close()
		return c.NATSUrl + " reachable", nil
	})

	// Doppler (optional)
	check("doppler", func() (string, error) {
		path, err := exec.LookPath("doppler")
		if err != nil {
			return "WARN:" + "not found (optional, needed for OTel secret injection)", nil
		}
		return path, nil
	})

	// Pre-commit hook
	check("pre-commit hook", func() (string, error) {
		hookPath := filepath.Join(".githooks", "pre-commit")
		if _, err := os.Stat(hookPath); err != nil {
			return "WARN:" + "not installed (run: make setup-hooks)", nil
		}
		// Check if git config points to .githooks
		out, err := exec.Command("git", "config", "core.hooksPath").Output()
		if err != nil || strings.TrimSpace(string(out)) != ".githooks" {
			return "WARN:" + ".githooks exists but git not configured (run: make setup-hooks)", nil
		}
		return "installed", nil
	})

	fmt.Println()
	fmt.Printf("=== %d passed, %d warnings, %d failed ===\n", passed, warned, failed)
	if failed > 0 {
		return fmt.Errorf("%d checks failed", failed)
	}
	return nil
}
