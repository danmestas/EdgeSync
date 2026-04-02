package agent

import (
	"context"
	"encoding/json"
	"fmt"
	"net"
	"net/http"
	"os"
	"os/exec"
	"syscall"
	"time"
)

// sidecar manages the iroh-sidecar child process lifecycle.
type sidecar struct {
	binPath     string
	socketPath  string
	keyPath     string
	callbackURL string
	alpn        string
	cmd         *exec.Cmd
	exited      chan struct{} // closed when the process exits
	exitErr     error        // set before exited is closed
}

// sidecarStatus is the JSON response from GET /status.
type sidecarStatus struct {
	EndpointID string  `json:"endpoint_id"`
	RelayURL   *string `json:"relay_url"`
}

// spawn starts the sidecar process.
func (s *sidecar) spawn() error {
	if _, err := os.Stat(s.binPath); err != nil {
		return fmt.Errorf("iroh sidecar binary not found: %w", err)
	}

	s.cmd = exec.Command(s.binPath,
		"--socket", s.socketPath,
		"--key-path", s.keyPath,
		"--callback", s.callbackURL,
		"--alpn", s.alpn,
	)
	s.cmd.Stdout = os.Stdout
	s.cmd.Stderr = os.Stderr

	if err := s.cmd.Start(); err != nil {
		return fmt.Errorf("iroh sidecar start: %w", err)
	}

	s.exited = make(chan struct{})
	go func() {
		s.exitErr = s.cmd.Wait()
		close(s.exited)
	}()
	return nil
}

// waitReady polls GET /status until the sidecar responds or timeout elapses.
func (s *sidecar) waitReady(timeout time.Duration) (*sidecarStatus, error) {
	client := &http.Client{
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
				return net.Dial("unix", s.socketPath)
			},
		},
		Timeout: 2 * time.Second,
	}

	deadline := time.Now().Add(timeout)
	for time.Now().Before(deadline) {
		resp, err := client.Get("http://iroh-sidecar/status")
		if err != nil {
			time.Sleep(200 * time.Millisecond)
			continue
		}

		if resp.StatusCode != http.StatusOK {
			resp.Body.Close()
			time.Sleep(200 * time.Millisecond)
			continue
		}

		var status sidecarStatus
		err = json.NewDecoder(resp.Body).Decode(&status)
		resp.Body.Close()
		if err != nil {
			time.Sleep(200 * time.Millisecond)
			continue
		}
		return &status, nil
	}

	return nil, fmt.Errorf("iroh sidecar not ready within %v", timeout)
}

// shutdown sends POST /shutdown and waits for the process to exit.
// Falls back to SIGTERM then SIGKILL.
func (s *sidecar) shutdown() {
	if s.cmd == nil || s.cmd.Process == nil {
		return
	}

	client := &http.Client{
		Transport: &http.Transport{
			DialContext: func(ctx context.Context, _, _ string) (net.Conn, error) {
				return net.Dial("unix", s.socketPath)
			},
		},
		Timeout: 2 * time.Second,
	}
	client.Post("http://iroh-sidecar/shutdown", "", nil) //nolint:errcheck

	select {
	case <-s.exited:
		s.cleanup()
		return
	case <-time.After(5 * time.Second):
	}

	s.cmd.Process.Signal(syscall.SIGTERM) //nolint:errcheck
	select {
	case <-s.exited:
		s.cleanup()
		return
	case <-time.After(2 * time.Second):
	}

	s.cmd.Process.Kill() //nolint:errcheck
	<-s.exited
	s.cleanup()
}

// kill forcefully terminates the sidecar process.
func (s *sidecar) kill() {
	if s.cmd != nil && s.cmd.Process != nil {
		s.cmd.Process.Kill() //nolint:errcheck
		<-s.exited
	}
	s.cleanup()
}

func (s *sidecar) cleanup() {
	os.Remove(s.socketPath) //nolint:errcheck
}
