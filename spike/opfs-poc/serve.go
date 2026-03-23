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
	"time"

	_ "github.com/dmestas/edgesync/go-libfossil/db/driver/ncruces"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	natsserver "github.com/nats-io/nats-server/v2/server"
	_ "github.com/ncruces/go-sqlite3/driver"
)

func main() {
	target := flag.String("target", "", "Fossil server URL to proxy clone requests to")
	repoPath := flag.String("repo", "", "Path to Fossil repo file (for remote-info)")
	natsPort := flag.Int("nats-port", 4222, "NATS TCP port")
	wsPort := flag.Int("ws-port", 8222, "NATS WebSocket port")
	flag.Parse()

	ns := startEmbeddedNATS(*natsPort, *wsPort)
	defer ns.Shutdown()

	dir := buildPlayground()
	defer os.RemoveAll(dir)

	mux := http.NewServeMux()
	if *target != "" {
		registerProxy(mux, *target)
	}
	if *repoPath != "" {
		registerRemoteInfo(mux, *repoPath)
	}
	registerStaticFiles(mux, dir)

	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		log.Fatal(err)
	}

	url := "http://" + ln.Addr().String()
	log.Printf("Playground ready at %s", url)
	if *target != "" {
		log.Printf("Proxying /proxy → %s", *target)
	}
	log.Println("Press Ctrl+C to stop")
	openBrowser(url)

	if err := http.Serve(ln, mux); err != nil && !errors.Is(err, net.ErrClosed) {
		log.Fatal(err)
	}
}

// startEmbeddedNATS boots a NATS server with TCP and WebSocket listeners.
func startEmbeddedNATS(tcpPort, wsPort int) *natsserver.Server {
	ns, err := natsserver.NewServer(&natsserver.Options{
		Port: tcpPort,
		Websocket: natsserver.WebsocketOpts{
			Port:  wsPort,
			NoTLS: true,
		},
	})
	if err != nil {
		log.Fatalf("NATS server: %v", err)
	}
	ns.Start()
	if !ns.ReadyForConnections(5 * time.Second) {
		log.Fatal("NATS server not ready after 5s")
	}
	log.Printf("NATS server: tcp://localhost:%d ws://localhost:%d", tcpPort, wsPort)
	return ns
}

// buildPlayground compiles the WASM binary and copies static assets to a temp dir.
func buildPlayground() string {
	dir, err := os.MkdirTemp("", "opfs-poc-*")
	if err != nil {
		log.Fatal(err)
	}
	log.Println("Building playground WASM...")
	if err := buildWASM(dir); err != nil {
		log.Fatalf("Build failed: %v", err)
	}
	if err := copyAssets(dir); err != nil {
		log.Fatalf("Copy assets failed: %v", err)
	}
	return dir
}

// registerProxy adds a /proxy endpoint that forwards xfer requests.
func registerProxy(mux *http.ServeMux, target string) {
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
		req, err := http.NewRequestWithContext(r.Context(), "POST", target, strings.NewReader(string(body)))
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
		if _, err := io.Copy(w, resp.Body); err != nil {
			log.Printf("proxy copy error: %v", err)
		}
	})
}

// registerRemoteInfo adds a /remote-info endpoint that queries a fossil repo.
func registerRemoteInfo(mux *http.ServeMux, repoPath string) {
	mux.HandleFunc("/remote-info", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Header().Set("Cross-Origin-Opener-Policy", "same-origin")
		w.Header().Set("Cross-Origin-Embedder-Policy", "require-corp")

		// Crosslink any new manifests so events are visible.
		if rp, err := repo.Open(repoPath); err == nil {
			if _, err := manifest.Crosslink(rp); err != nil {
				log.Printf("remote-info crosslink: %v", err)
			}
			rp.Close()
		}

		info := queryRepoInfo(repoPath)
		json.NewEncoder(w).Encode(info)
	})
}

// queryRepoInfo reads blob/event counts and recent timeline from a repo.
func queryRepoInfo(repoPath string) map[string]any {
	db, err := sql.Open("sqlite3", "file:"+repoPath)
	if err != nil {
		return map[string]any{"error": err.Error()}
	}
	defer db.Close()

	var blobCount, checkinCount int
	_ = db.QueryRow("SELECT count(*) FROM blob").Scan(&blobCount)
	_ = db.QueryRow("SELECT count(*) FROM event WHERE type='ci'").Scan(&checkinCount)

	var projectCode string
	_ = db.QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projectCode)

	type checkin struct {
		UUID    string `json:"uuid"`
		User    string `json:"user"`
		Comment string `json:"comment"`
		Time    string `json:"time"`
	}
	var checkins []checkin
	rows, err := db.Query(`
		SELECT b.uuid, e.user, e.comment, datetime(e.mtime)
		FROM event e JOIN blob b ON b.rid=e.objid
		WHERE e.type='ci'
		ORDER BY e.mtime DESC LIMIT 10
	`)
	if err == nil {
		defer rows.Close()
		for rows.Next() {
			var c checkin
			if err := rows.Scan(&c.UUID, &c.User, &c.Comment, &c.Time); err != nil {
				continue
			}
			checkins = append(checkins, c)
		}
	}

	return map[string]any{
		"blobs":       blobCount,
		"checkins":    checkinCount,
		"projectCode": projectCode,
		"timeline":    checkins,
	}
}

// registerStaticFiles serves static files with COOP/COEP headers.
func registerStaticFiles(mux *http.ServeMux, dir string) {
	fs := http.FileServer(http.Dir(dir))
	mux.HandleFunc("/", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Cross-Origin-Opener-Policy", "same-origin")
		w.Header().Set("Cross-Origin-Embedder-Policy", "require-corp")
		fs.ServeHTTP(w, r)
	})
}

func buildWASM(dir string) error {
	out := filepath.Join(dir, "poc.wasm")
	cmd := exec.Command("go", "build", "-buildvcs=false", "-o", out, ".")
	cmd.Env = append(os.Environ(), "GOOS=js", "GOARCH=wasm")
	cmd.Stdout = os.Stdout
	cmd.Stderr = os.Stderr
	if err := cmd.Run(); err != nil {
		return fmt.Errorf("go build: %w", err)
	}
	return copyWasmExecJS(dir)
}

func copyWasmExecJS(dir string) error {
	goroot := os.Getenv("GOROOT")
	if goroot == "" {
		out, err := exec.Command("go", "env", "GOROOT").Output()
		if err != nil {
			return fmt.Errorf("go env GOROOT: %w", err)
		}
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
	return fmt.Errorf("wasm_exec.js not found in GOROOT=%s", goroot)
}

func copyAssets(dir string) error {
	for _, name := range []string{"index.html", "worker.js"} {
		data, err := os.ReadFile(name)
		if err != nil {
			return fmt.Errorf("read %s: %w", name, err)
		}
		if err := os.WriteFile(filepath.Join(dir, name), data, 0644); err != nil {
			return fmt.Errorf("write %s: %w", name, err)
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
	if err := exec.Command(cmd, url).Start(); err != nil {
		log.Printf("open browser: %v", err)
	}
}
