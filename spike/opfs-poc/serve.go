//go:build !js

package main

import (
	"database/sql"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"io"
	"log"
	"net"
	"net/http"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"strings"

	_ "github.com/dmestas/edgesync/go-libfossil/db/driver/ncruces"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	_ "github.com/ncruces/go-sqlite3/driver"
)

func main() {
	target := flag.String("target", "https://sync.craftdesign.group", "Fossil server URL to proxy to")
	repoPath := flag.String("repo", "", "Path to Fossil repo file (for remote-info endpoint)")
	flag.Parse()

	dir, err := os.MkdirTemp("", "opfs-poc-*")
	if err != nil {
		log.Fatal(err)
	}
	defer os.RemoveAll(dir)

	log.Println("Building PoC WASM...")
	if err := buildWASM(dir); err != nil {
		log.Fatalf("Build failed: %v", err)
	}

	if err := copyAssets(dir); err != nil {
		log.Fatalf("Copy assets failed: %v", err)
	}

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		log.Fatal(err)
	}

	url := "http://" + ln.Addr().String()
	log.Printf("Playground ready at %s", url)
	log.Printf("Proxying /proxy → %s", *target)
	if *repoPath != "" {
		log.Printf("Remote info from %s", *repoPath)
	}
	log.Println("Press Ctrl+C to stop")

	openBrowser(url)

	fs := http.FileServer(http.Dir(dir))
	mux := http.NewServeMux()

	// Proxy endpoint — forwards xfer requests to the real Fossil server.
	mux.HandleFunc("/proxy", func(w http.ResponseWriter, r *http.Request) {
		if r.Method != "POST" {
			http.Error(w, "POST only", http.StatusMethodNotAllowed)
			return
		}
		body, err := io.ReadAll(r.Body)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadRequest)
			return
		}

		req, err := http.NewRequestWithContext(r.Context(), "POST", *target, strings.NewReader(string(body)))
		if err != nil {
			http.Error(w, err.Error(), http.StatusInternalServerError)
			return
		}
		req.Header.Set("Content-Type", "application/x-fossil")

		resp, err := http.DefaultClient.Do(req)
		if err != nil {
			http.Error(w, err.Error(), http.StatusBadGateway)
			return
		}
		defer resp.Body.Close()

		w.Header().Set("Content-Type", resp.Header.Get("Content-Type"))
		w.WriteHeader(resp.StatusCode)
		io.Copy(w, resp.Body)
	})

	// Remote info — queries the fossil repo directly.
	mux.HandleFunc("/remote-info", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cross-Origin-Opener-Policy", "same-origin")
		w.Header().Set("Cross-Origin-Embedder-Policy", "require-corp")

		if *repoPath == "" {
			json.NewEncoder(w).Encode(map[string]any{"error": "no -repo flag specified"})
			return
		}

		// Crosslink any new manifests before querying.
		if r, err := repo.Open(*repoPath); err == nil {
			manifest.Crosslink(r)
			r.Close()
		}

		db, err := sql.Open("sqlite3", "file:"+*repoPath)
		if err != nil {
			json.NewEncoder(w).Encode(map[string]any{"error": err.Error()})
			return
		}
		defer db.Close()

		var blobCount, checkinCount int
		db.QueryRow("SELECT count(*) FROM blob").Scan(&blobCount)
		db.QueryRow("SELECT count(*) FROM event WHERE type='ci'").Scan(&checkinCount)

		var projectCode string
		db.QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projectCode)

		// Recent checkins.
		rows, err := db.Query(`
			SELECT b.uuid, e.user, e.comment, datetime(e.mtime)
			FROM event e JOIN blob b ON b.rid=e.objid
			WHERE e.type='ci'
			ORDER BY e.mtime DESC LIMIT 10
		`)
		type checkin struct {
			UUID    string `json:"uuid"`
			User    string `json:"user"`
			Comment string `json:"comment"`
			Time    string `json:"time"`
		}
		var checkins []checkin
		if err == nil {
			defer rows.Close()
			for rows.Next() {
				var c checkin
				rows.Scan(&c.UUID, &c.User, &c.Comment, &c.Time)
				checkins = append(checkins, c)
			}
		}

		json.NewEncoder(w).Encode(map[string]any{
			"blobs":       blobCount,
			"checkins":    checkinCount,
			"projectCode": projectCode,
			"timeline":    checkins,
		})
	})

	// Static files with COOP/COEP headers.
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cross-Origin-Opener-Policy", "same-origin")
		w.Header().Set("Cross-Origin-Embedder-Policy", "require-corp")
		fs.ServeHTTP(w, r)
	})

	if err := http.Serve(ln, mux); err != nil && !errors.Is(err, net.ErrClosed) {
		log.Fatal(err)
	}
}

func buildWASM(dir string) error {
	out := filepath.Join(dir, "poc.wasm")
	cmd := exec.Command("go", "build", "-buildvcs=false", "-tags", "ncruces", "-o", out, ".")
	cmd.Env = append(os.Environ(), "GOOS=js", "GOARCH=wasm")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("go build: %w", err)
	}

	goroot := os.Getenv("GOROOT")
	if goroot == "" {
		out, _ := exec.Command("go", "env", "GOROOT").Output()
		goroot = strings.TrimSpace(string(out))
	}
	for _, p := range []string{
		filepath.Join(goroot, "lib", "wasm", "wasm_exec.js"),
		filepath.Join(goroot, "misc", "wasm", "wasm_exec.js"),
	} {
		data, err := os.ReadFile(p)
		if err == nil {
			return os.WriteFile(filepath.Join(dir, "wasm_exec.js"), data, 0644)
		}
	}
	return fmt.Errorf("wasm_exec.js not found in GOROOT")
}

func copyAssets(dir string) error {
	for _, name := range []string{"index.html", "worker.js"} {
		data, err := os.ReadFile(name)
		if err != nil {
			return err
		}
		if err := os.WriteFile(filepath.Join(dir, name), data, 0644); err != nil {
			return err
		}
	}
	return nil
}

func openBrowser(url string) {
	var cmd string
	switch runtime.GOOS {
	case "darwin":
		cmd = "open"
	case "linux":
		cmd = "xdg-open"
	case "windows":
		cmd = "start"
	default:
		return
	}
	exec.Command(cmd, url).Start()
}
