# EdgeSync Notify App — Phase 3-4: gomobile Framework + Expo Scaffolding

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Compile the Go HTTP server into an iOS/macOS framework via gomobile, then scaffold the Expo app with routing, native bridge, typed API client, and stub screens.

**Architecture:** `go/main.go` exports `Start(dataDir) int`, `Stop()`, `IsRunning() bool` for gomobile bind. On failure, `Start()` writes a human-readable error to `<dataDir>/server-error` so the native module can surface it. A build script produces `NotifyBridge.xcframework`. The Expo app uses file-based routing (Expo Router), a Swift native module to call Go's Start/Stop, and a deep `api.ts` module created via `createApi(port)` that handles all HTTP/SSE communication with the localhost Go server. The port is required at construction time -- screens access the API via a React context (`useApi()` hook), making the dependency explicit at the type level. Components never see raw HTTP.

**Tech Stack:** Go 1.26, gomobile, Expo SDK 53, Expo Router, React Native, TypeScript, Swift

**Spec:** `docs/superpowers/specs/2026-04-10-notify-expo-app-design.md`

**Prerequisite:** Phase 2 (Go HTTP server) must be complete. The following files must exist in `edgesync-notify-app/go/`:
- `server.go` — localhost HTTP + SSE server (`NewServer`, `ListenAndServe`, `Shutdown`)
- `bridge.go` — HTTP handlers wrapping `notify.Service`
- `sse.go` — SSE streaming implementation
- `pair.go` — pairing token decode + hub connection
- `go.mod` — imports `go-libfossil` and `edgesync/leaf/agent/notify`

**Repo:** `edgesync-notify-app` (standalone GitHub repo)

**No Claude attribution on commits.**

---

## File Structure

### Phase 3: gomobile Framework

| Action | File | Responsibility |
|--------|------|----------------|
| Create | `go/main.go` | gomobile-exported functions: `Start`, `Stop`, `IsRunning`. Manages server lifecycle and port file. |
| Create | `go/main_test.go` | Tests for Start/Stop lifecycle, port file writing, double-start safety |
| Create | `scripts/build-go.sh` | Shell script wrapping `gomobile bind` for iOS and macOS targets |

### Phase 4: Expo App Scaffolding

| Action | File | Responsibility |
|--------|------|----------------|
| Create | `app.json` | Expo config: app name, iOS + macOS targets, bundle ID |
| Create | `package.json` | Dependencies: expo, expo-router, react-native, TypeScript |
| Create | `tsconfig.json` | TypeScript config for Expo |
| Create | `app/_layout.tsx` | Root layout with Expo Router Stack navigator |
| Create | `app/(screens)/index.tsx` | Inbox screen — thread list, pull-to-refresh, SSE updates |
| Create | `app/(screens)/thread/[id].tsx` | Thread detail — message bubbles, action buttons, reply composer |
| Create | `app/(screens)/settings.tsx` | Settings screen — hub status, device name, disconnect |
| Create | `app/lib/types.ts` | TypeScript types matching Go JSON: Message, ThreadSummary, Action, Priority, SSE events |
| Create | `app/lib/api.ts` | Deep module: typed fetch wrapper, EventSource with auto-reconnect, error normalization, connection status |
| Create | `app/components/ConnectionStatus.tsx` | Shared `ConnectionStatusBar` component + `formatConnectionStatus()` utility |
| Create | `app/components/ThreadRow.tsx` | Single thread row for inbox list (includes co-located PriorityBadge) |
| Create | `app/components/MessageBubble.tsx` | Message bubble (left/right aligned by sender, includes co-located ActionButton) |
| Create | `app/components/ReplyComposer.tsx` | Text input + send button pinned at bottom |
| Create | `app/components/PairingScreen.tsx` | First-launch flow: scan QR or enter token |
| Create | `ios/NotifyBridge.swift` | Native module: calls Go Start/Stop, exposes port to JS |

---

## Task 1: gomobile Export Functions

**Files:**
- Create: `edgesync-notify-app/go/main.go`
- Create: `edgesync-notify-app/go/main_test.go`

- [ ] **Step 1: Write the failing test for Start/Stop lifecycle**

Create `go/main_test.go`:

```go
package notifyapp

import (
	"fmt"
	"net/http"
	"os"
	"path/filepath"
	"testing"
	"time"
)

func TestStartReturnsValidPort(t *testing.T) {
	dir := t.TempDir()
	port := Start(dir)
	defer Stop()

	if port <= 0 || port > 65535 {
		t.Fatalf("Start returned invalid port: %d", port)
	}

	// Server should be reachable.
	resp, err := http.Get(fmt.Sprintf("http://127.0.0.1:%d/status", port))
	if err != nil {
		t.Fatalf("server not reachable: %v", err)
	}
	resp.Body.Close()
	if resp.StatusCode != http.StatusOK {
		t.Errorf("status code = %d, want 200", resp.StatusCode)
	}
}

func TestStartWritesPortFile(t *testing.T) {
	dir := t.TempDir()
	port := Start(dir)
	defer Stop()

	data, err := os.ReadFile(filepath.Join(dir, "server-port"))
	if err != nil {
		t.Fatalf("read port file: %v", err)
	}
	want := fmt.Sprintf("%d", port)
	if string(data) != want {
		t.Errorf("port file = %q, want %q", string(data), want)
	}
}

func TestIsRunning(t *testing.T) {
	if IsRunning() {
		t.Error("IsRunning should be false before Start")
	}

	dir := t.TempDir()
	Start(dir)

	if !IsRunning() {
		t.Error("IsRunning should be true after Start")
	}

	Stop()

	// Give the server a moment to shut down.
	time.Sleep(50 * time.Millisecond)

	if IsRunning() {
		t.Error("IsRunning should be false after Stop")
	}
}

func TestDoubleStartReturnsExistingPort(t *testing.T) {
	dir := t.TempDir()
	port1 := Start(dir)
	defer Stop()

	port2 := Start(dir)
	if port1 != port2 {
		t.Errorf("double Start returned different ports: %d vs %d", port1, port2)
	}
}

func TestStopWithoutStartIsNoop(t *testing.T) {
	// Should not panic.
	Stop()
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `cd edgesync-notify-app && go test ./go/ -v -count=1`
Expected: FAIL — `Start`, `Stop`, `IsRunning` undefined

- [ ] **Step 3: Write the implementation**

Create `go/main.go`:

```go
// Package notifyapp provides gomobile-exported functions for the EdgeSync Notify app.
//
// The framework runs a localhost HTTP server inside the iOS/macOS app process.
// React Native communicates with it via fetch (request/response) and EventSource (SSE).
//
// Exported functions (gomobile bind):
//   - Start(dataDir string) int — start server, return port
//   - Stop() — shut down server
//   - IsRunning() bool — health check
package notifyapp

import (
	"context"
	"fmt"
	"net"
	"net/http"
	"os"
	"path/filepath"
	"sync"
)

var (
	mu      sync.Mutex
	srv     *http.Server
	port    int
	cancel  context.CancelFunc
	running bool
)

// Start starts the HTTP server on 127.0.0.1 with a random available port.
// It writes the port number to <dataDir>/server-port and returns it.
// If the server is already running, returns the existing port.
// dataDir is used for notify.fossil, iroh keys, and the port file.
func Start(dataDir string) int {
	mu.Lock()
	defer mu.Unlock()

	if running {
		return port
	}

	// Ensure data directory exists before anything else.
	if err := os.MkdirAll(dataDir, 0o755); err != nil {
		writeError(dataDir, fmt.Sprintf("mkdir: %v", err))
		return 0
	}

	// Bind to a random available port.
	ln, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		// gomobile exports can't return errors — return 0 to signal failure.
		// Write the error to <dataDir>/server-error so the native module can surface it.
		writeError(dataDir, fmt.Sprintf("listen: %v", err))
		return 0
	}

	port = ln.Addr().(*net.TCPAddr).Port

	// Write port file so other processes can discover it.
	portFile := filepath.Join(dataDir, "server-port")
	if err := os.WriteFile(portFile, []byte(fmt.Sprintf("%d", port)), 0o644); err != nil {
		ln.Close()
		writeError(dataDir, fmt.Sprintf("write port file: %v", err))
		return 0
	}

	// Create the server using the Phase 2 NewServer constructor.
	// NewServer returns an *http.ServeMux with all routes registered.
	mux := NewServer(dataDir)

	var ctx context.Context
	ctx, cancel = context.WithCancel(context.Background())

	srv = &http.Server{Handler: mux}

	go func() {
		if err := srv.Serve(ln); err != nil && err != http.ErrServerClosed {
			// Log but don't crash — the app can show "disconnected" state.
			fmt.Fprintf(os.Stderr, "notifyapp: server error: %v\n", err)
		}
	}()

	// Wait for context cancellation to trigger shutdown.
	go func() {
		<-ctx.Done()
		srv.Shutdown(context.Background())
	}()

	running = true
	return port
}

// Stop shuts down the HTTP server and disconnects NATS/iroh.
// Safe to call multiple times or without a prior Start.
func Stop() {
	mu.Lock()
	defer mu.Unlock()

	if !running {
		return
	}

	cancel()
	running = false
	port = 0
}

// IsRunning returns true if the HTTP server is currently running.
func IsRunning() bool {
	mu.Lock()
	defer mu.Unlock()
	return running
}

// writeError writes an error string to <dataDir>/server-error so the native module
// can read it and surface a meaningful message when Start() returns 0.
func writeError(dataDir, msg string) {
	errFile := filepath.Join(dataDir, "server-error")
	_ = os.WriteFile(errFile, []byte(msg), 0o644)
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `cd edgesync-notify-app && go test ./go/ -v -count=1`
Expected: PASS (all 5 tests)

Note: This requires `NewServer(dataDir string) *http.ServeMux` to exist in `server.go` (Phase 2). If Phase 2 is not yet complete, stub it:

```go
// Temporary stub in server.go — remove when Phase 2 is complete.
func NewServer(dataDir string) *http.ServeMux {
	mux := http.NewServeMux()
	mux.HandleFunc("/status", func(w http.ResponseWriter, r *http.Request) {
		w.Header().Set("Content-Type", "application/json")
		w.Write([]byte(`{"connected":false,"hub":"","peer_id":""}`))
	})
	return mux
}
```

- [ ] **Step 5: Commit**

```bash
git add go/main.go go/main_test.go
git commit -m "feat: gomobile export functions — Start, Stop, IsRunning"
```

---

## Task 2: gomobile Build Script

**Files:**
- Create: `edgesync-notify-app/scripts/build-go.sh`

- [ ] **Step 1: Create the build script**

Create `scripts/build-go.sh`:

```bash
#!/usr/bin/env bash
set -euo pipefail

# Build the Go module into iOS/macOS xcframeworks via gomobile bind.
#
# Prerequisites:
#   go install golang.org/x/mobile/cmd/gomobile@latest
#   gomobile init
#
# Usage:
#   ./scripts/build-go.sh          # build both iOS and macOS
#   ./scripts/build-go.sh ios      # build iOS only
#   ./scripts/build-go.sh macos    # build macOS only

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(dirname "$SCRIPT_DIR")"
GO_DIR="$ROOT_DIR/go"
IOS_OUT="$ROOT_DIR/ios/NotifyBridge.xcframework"
MACOS_OUT="$ROOT_DIR/macos/NotifyBridge.xcframework"

TARGET="${1:-all}"

# Verify gomobile is installed.
if ! command -v gomobile &>/dev/null; then
    echo "Error: gomobile not found. Install it with:"
    echo "  go install golang.org/x/mobile/cmd/gomobile@latest && gomobile init"
    exit 1
fi

cd "$GO_DIR"

build_ios() {
    echo "Building iOS xcframework..."
    rm -rf "$IOS_OUT"
    gomobile bind -target ios -o "$IOS_OUT" .
    echo "Built: $IOS_OUT"
}

build_macos() {
    echo "Building macOS xcframework..."
    rm -rf "$MACOS_OUT"
    gomobile bind -target macos -o "$MACOS_OUT" .
    echo "Built: $MACOS_OUT"
}

case "$TARGET" in
    ios)   build_ios ;;
    macos) build_macos ;;
    all)   build_ios; build_macos ;;
    *)     echo "Usage: $0 [ios|macos|all]"; exit 1 ;;
esac

echo "Done."
```

- [ ] **Step 2: Make the script executable**

Run: `chmod +x edgesync-notify-app/scripts/build-go.sh`

- [ ] **Step 3: Verify gomobile bind compiles (iOS target)**

Run: `cd edgesync-notify-app && ./scripts/build-go.sh ios`
Expected: `NotifyBridge.xcframework` created in `ios/`. The framework exports `NotifyappStart`, `NotifyappStop`, `NotifyappIsRunning` (gomobile prefixes package name).

If gomobile is not installed, install it first:
```bash
go install golang.org/x/mobile/cmd/gomobile@latest
gomobile init
```

- [ ] **Step 4: Verify the framework exports**

Run: `nm -g ios/NotifyBridge.xcframework/ios-arm64/NotifyBridge.framework/NotifyBridge 2>/dev/null | grep -i notifyapp || echo "Check framework structure manually"`

Look for symbols containing `Start`, `Stop`, `IsRunning`.

- [ ] **Step 5: Add xcframework to .gitignore**

Append to `edgesync-notify-app/.gitignore`:

```
# Built frameworks (regenerated by scripts/build-go.sh)
ios/NotifyBridge.xcframework/
macos/NotifyBridge.xcframework/
```

- [ ] **Step 6: Commit**

```bash
git add scripts/build-go.sh .gitignore
git commit -m "feat: gomobile bind build script for iOS and macOS"
```

---

## Task 3: Expo Project Scaffolding

**Files:**
- Create: `edgesync-notify-app/app.json`
- Create: `edgesync-notify-app/package.json`
- Create: `edgesync-notify-app/tsconfig.json`
- Create: `edgesync-notify-app/app/_layout.tsx`

- [ ] **Step 1: Initialize the Expo project**

Run from the repo root (`edgesync-notify-app/`):

```bash
npx create-expo-app@latest . --template blank-typescript --no-install
```

If the directory already has files, create-expo-app may refuse. In that case, create in a temp dir and move:

```bash
npx create-expo-app@latest /tmp/notify-expo --template blank-typescript --no-install
cp /tmp/notify-expo/tsconfig.json .
# Merge package.json dependencies manually
```

- [ ] **Step 2: Install dependencies**

```bash
npx expo install expo-router react-native-safe-area-context react-native-screens expo-linking expo-constants expo-status-bar
npm install
```

- [ ] **Step 3: Configure app.json for iOS + macOS with Expo Router**

Replace `edgesync-notify-app/app.json`:

```json
{
  "expo": {
    "name": "EdgeSync Notify",
    "slug": "edgesync-notify",
    "version": "0.1.0",
    "scheme": "edgesync-notify",
    "platforms": ["ios"],
    "ios": {
      "bundleIdentifier": "group.craftdesign.edgesync.notify",
      "supportsTablet": true
    },
    "plugins": [
      "expo-router"
    ],
    "experiments": {
      "typedRoutes": true
    }
  }
}
```

- [ ] **Step 4: Create the root layout with Expo Router**

Create `app/_layout.tsx`:

```tsx
import { Stack } from "expo-router";

export default function RootLayout() {
  return (
    <Stack
      screenOptions={{
        headerStyle: { backgroundColor: "#ffffff" },
        headerTintColor: "#000000",
        headerTitleStyle: { fontWeight: "600" },
        contentStyle: { backgroundColor: "#ffffff" },
      }}
    >
      <Stack.Screen name="(screens)" options={{ headerShown: false }} />
    </Stack>
  );
}
```

- [ ] **Step 5: Create the screens layout**

Create `app/(screens)/_layout.tsx`:

```tsx
import { Stack } from "expo-router";

export default function ScreensLayout() {
  return (
    <Stack>
      <Stack.Screen name="index" options={{ title: "Inbox" }} />
      <Stack.Screen name="thread/[id]" options={{ title: "Thread" }} />
      <Stack.Screen name="settings" options={{ title: "Settings" }} />
    </Stack>
  );
}
```

- [ ] **Step 6: Verify TypeScript compiles**

Run: `cd edgesync-notify-app && npx tsc --noEmit`
Expected: No errors (may have warnings about missing screen files — that's fine, we create them in Task 6)

- [ ] **Step 7: Commit**

```bash
git add app.json package.json tsconfig.json app/_layout.tsx app/\(screens\)/_layout.tsx
git commit -m "feat: expo project scaffolding with Expo Router"
```

---

## Task 4: TypeScript Types

**Files:**
- Create: `edgesync-notify-app/app/lib/types.ts`

- [ ] **Step 1: Create types matching Go JSON struct tags**

Create `app/lib/types.ts`:

```ts
// Types matching Go JSON wire format from the localhost server.
// Source of truth: leaf/agent/notify/message.go (Message struct tags)
// and go/bridge.go (ThreadSummary, StatusResponse).

export type Priority = "info" | "action_required" | "urgent";

export interface Action {
  id: string;
  label: string;
}

export interface Message {
  v: number;
  id: string;
  thread: string;
  project: string;
  from: string;
  from_name: string;
  timestamp: string; // ISO 8601 — Go's time.Time marshals to RFC3339
  body: string;
  priority?: Priority;
  actions?: Action[];
  reply_to?: string;
  media?: string[];
  action_response?: boolean;
}

export interface ThreadSummary {
  thread_short: string;
  project: string;
  last_activity: string; // ISO 8601
  message_count: number;
  last_message: Message;
  priority: Priority;
}

export interface StatusResponse {
  connected: boolean;
  hub: string;
  peer_id: string;
}

export interface SendRequest {
  project: string;
  body: string;
  priority?: Priority;
  actions?: Action[];
  thread?: string; // Reply to existing thread
}

export interface SendResponse {
  message: Message;
}

export interface InitRequest {
  hub: string;
  iroh_peers?: string[];
  device_name: string;
}

export interface PairRequest {
  token: string;
}

export interface PairResponse {
  status: "ok";
  hub: string;
}

export interface ErrorResponse {
  error: string;
}

// SSE event types from GET /subscribe
export type SSEEvent =
  | { type: "message"; data: Message }
  | { type: "connected"; data: { hub: string; peer_id: string } }
  | { type: "disconnected"; data: { reason: string } };

export type ConnectionStatus = "connected" | "disconnected" | "connecting";

// Default project — single constant, imported by screens that need it.
export const DEFAULT_PROJECT = "edgesync";
```

- [ ] **Step 2: Verify TypeScript compiles**

Run: `cd edgesync-notify-app && npx tsc --noEmit`
Expected: No errors

- [ ] **Step 3: Commit**

```bash
git add app/lib/types.ts
git commit -m "feat: TypeScript types matching Go JSON wire format"
```

---

## Task 5: API Client (Deep Module)

**Files:**
- Create: `edgesync-notify-app/app/lib/api.ts`

This is the deep module. It owns all HTTP communication, SSE reconnection, error normalization, and connection status tracking. Components never import `fetch` or `EventSource` directly.

The module exports a `createApi(port)` factory that returns a typed API object. The port is required at construction time, so callers cannot invoke API methods without providing it. This makes the dependency explicit at the type level instead of a runtime error.

- [ ] **Step 1: Create the API client**

Create `app/lib/api.ts`:

```ts
// Deep module: all HTTP/SSE communication with the Go localhost server.
//
// Usage: const api = createApi(port)
// The port is required at construction — you can't call API methods without it.
//
// This module handles:
//   - Base URL from the port provided at creation
//   - Error normalization (network errors, HTTP errors, JSON parse errors → ApiError)
//   - SSE auto-reconnect with exponential backoff
//   - Connection status tracking
//
// No raw fetch or EventSource escapes this module.

import {
  ConnectionStatus,
  ErrorResponse,
  InitRequest,
  Message,
  PairRequest,
  PairResponse,
  SendRequest,
  SendResponse,
  SSEEvent,
  StatusResponse,
  ThreadSummary,
} from "./types";

// --- Error normalization ---

export class ApiError extends Error {
  constructor(
    message: string,
    public readonly status?: number,
    public readonly code?: string,
  ) {
    super(message);
    this.name = "ApiError";
  }
}

// --- API interface ---

export interface Api {
  readonly port: number;
  getConnectionStatus(): ConnectionStatus;
  onConnectionStatusChange(listener: (status: ConnectionStatus) => void): () => void;
  init(opts: InitRequest): Promise<void>;
  getStatus(): Promise<StatusResponse>;
  getThreads(project: string): Promise<ThreadSummary[]>;
  getThread(project: string, threadId: string): Promise<Message[]>;
  sendMessage(opts: SendRequest): Promise<SendResponse>;
  pair(opts: PairRequest): Promise<PairResponse>;
  stop(): Promise<void>;
  mediaUrl(project: string, filename: string): string;
  subscribe(project: string, onEvent: (event: SSEEvent) => void): Subscription;
}

// --- Factory ---

export function createApi(port: number): Api {
  const baseUrl = `http://127.0.0.1:${port}`;

  // --- Connection status (scoped to this instance) ---

  type StatusListener = (status: ConnectionStatus) => void;

  let currentStatus: ConnectionStatus = "disconnected";
  const statusListeners = new Set<StatusListener>();

  function setStatus(status: ConnectionStatus) {
    if (status === currentStatus) return;
    currentStatus = status;
    statusListeners.forEach((fn) => fn(status));
  }

  // --- HTTP helpers ---

  async function request<T>(
    method: string,
    path: string,
    body?: unknown,
  ): Promise<T> {

  let resp: Response;
  try {
    resp = await fetch(`${baseUrl}${path}`, {
      method,
      headers: body ? { "Content-Type": "application/json" } : undefined,
      body: body ? JSON.stringify(body) : undefined,
    });
  } catch (err) {
    setStatus("disconnected");
    throw new ApiError(
      `Network error: ${err instanceof Error ? err.message : String(err)}`,
      undefined,
      "NETWORK",
    );
  }

  if (!resp.ok) {
    let errorMsg = `HTTP ${resp.status}`;
    try {
      const errBody: ErrorResponse = await resp.json();
      errorMsg = errBody.error || errorMsg;
    } catch {
      // Response wasn't JSON — use status text.
    }
    throw new ApiError(errorMsg, resp.status, "HTTP");
  }

  // 204 No Content or empty body.
  const text = await resp.text();
  if (!text) return {} as T;

  try {
    return JSON.parse(text) as T;
  } catch {
    throw new ApiError("Invalid JSON response", resp.status, "PARSE");
  }
}

// --- Build and return the API object ---

  return {
    port,

    getConnectionStatus(): ConnectionStatus {
      return currentStatus;
    },

    onConnectionStatusChange(listener: StatusListener): () => void {
      statusListeners.add(listener);
      return () => statusListeners.delete(listener);
    },

    async init(opts: InitRequest): Promise<void> {
      await request<{ status: string }>("POST", "/init", opts);
    },

    async getStatus(): Promise<StatusResponse> {
      return request<StatusResponse>("GET", "/status");
    },

    async getThreads(project: string): Promise<ThreadSummary[]> {
      return request<ThreadSummary[]>("GET", `/threads?project=${encodeURIComponent(project)}`);
    },

    async getThread(project: string, threadId: string): Promise<Message[]> {
      return request<Message[]>("GET", `/thread/${encodeURIComponent(threadId)}?project=${encodeURIComponent(project)}`);
    },

    async sendMessage(opts: SendRequest): Promise<SendResponse> {
      return request<SendResponse>("POST", "/send", opts);
    },

    async pair(opts: PairRequest): Promise<PairResponse> {
      return request<PairResponse>("POST", "/pair", opts);
    },

    async stop(): Promise<void> {
      await request<{ status: string }>("POST", "/stop");
      setStatus("disconnected");
    },

    mediaUrl(project: string, filename: string): string {
      return `${baseUrl}/media/${encodeURIComponent(filename)}?project=${encodeURIComponent(project)}`;
    },

// --- SSE subscription with auto-reconnect ---

const RECONNECT_BASE_MS = 1000;
const RECONNECT_MAX_MS = 30000;

export interface Subscription {
  close(): void;
}

    subscribe(
      project: string,
      onEvent: (event: SSEEvent) => void,
    ): Subscription {
  let closed = false;
  let reconnectDelay = RECONNECT_BASE_MS;
  let timeoutId: ReturnType<typeof setTimeout> | null = null;
  let currentEventSource: EventSource | null = null;

  function connect() {
    if (closed) return;

    setStatus("connecting");
    const url = `${baseUrl}/subscribe?project=${encodeURIComponent(project)}`;
    const es = new EventSource(url);
    currentEventSource = es;

    es.onopen = () => {
      setStatus("connected");
      reconnectDelay = RECONNECT_BASE_MS; // Reset backoff on success.
    };

    es.addEventListener("message", (e: MessageEvent) => {
      try {
        const msg: Message = JSON.parse(e.data);
        onEvent({ type: "message", data: msg });
      } catch {
        // Skip malformed events.
      }
    });

    es.addEventListener("connected", (e: MessageEvent) => {
      try {
        onEvent({ type: "connected", data: JSON.parse(e.data) });
        setStatus("connected");
      } catch {
        // Skip malformed events.
      }
    });

    es.addEventListener("disconnected", (e: MessageEvent) => {
      try {
        onEvent({ type: "disconnected", data: JSON.parse(e.data) });
        setStatus("disconnected");
      } catch {
        // Skip malformed events.
      }
    });

    es.onerror = () => {
      es.close();
      currentEventSource = null;
      setStatus("disconnected");

      if (closed) return;

      // Exponential backoff with jitter.
      const jitter = Math.random() * 0.3 * reconnectDelay;
      const delay = Math.min(reconnectDelay + jitter, RECONNECT_MAX_MS);
      reconnectDelay = Math.min(reconnectDelay * 2, RECONNECT_MAX_MS);

      timeoutId = setTimeout(connect, delay);
    };
  }

  connect();

      return {
        close() {
          closed = true;
          if (timeoutId) clearTimeout(timeoutId);
          if (currentEventSource) {
            currentEventSource.close();
            currentEventSource = null;
          }
        },
      };
    },
  };
}
```

- [ ] **Step 2: Verify TypeScript compiles**

Run: `cd edgesync-notify-app && npx tsc --noEmit`
Expected: No errors

- [ ] **Step 3: Commit**

```bash
git add app/lib/api.ts
git commit -m "feat: deep API client with SSE auto-reconnect and error normalization"
```

---

## Task 6: Stub Screens

**Files:**
- Create: `edgesync-notify-app/app/(screens)/index.tsx`
- Create: `edgesync-notify-app/app/(screens)/thread/[id].tsx`
- Create: `edgesync-notify-app/app/(screens)/settings.tsx`

- [ ] **Step 1: Create the Inbox screen stub**

Create `app/(screens)/index.tsx`:

```tsx
import { useCallback, useEffect, useState } from "react";
import {
  FlatList,
  RefreshControl,
  StyleSheet,
  Text,
  View,
} from "react-native";
import { useRouter } from "expo-router";

import { useApi } from "../_layout";
import { ApiError } from "../lib/api";
import { ConnectionStatus, DEFAULT_PROJECT, SSEEvent, ThreadSummary } from "../lib/types";
import { ThreadRow } from "../components/ThreadRow";
import { ConnectionStatusBar } from "../components/ConnectionStatus";
import { PairingScreen } from "../components/PairingScreen";

export default function InboxScreen() {
  const router = useRouter();
  const api = useApi();
  const [threads, setThreads] = useState<ThreadSummary[]>([]);
  const [refreshing, setRefreshing] = useState(false);
  const [connectionStatus, setConnectionStatus] =
    useState<ConnectionStatus>("disconnected");
  const [paired, setPaired] = useState<boolean | null>(null); // null = checking

  // Check pairing status on mount.
  useEffect(() => {
    api
      .getStatus()
      .then((s) => {
        setPaired(true);
        setConnectionStatus(s.connected ? "connected" : "disconnected");
      })
      .catch(() => setPaired(false));
  }, []);

  // Load threads.
  const loadThreads = useCallback(async () => {
    try {
      const data = await api.getThreads(DEFAULT_PROJECT);
      setThreads(data);
    } catch {
      // Show empty state on error.
    }
  }, []);

  useEffect(() => {
    if (!paired) return;
    loadThreads();
  }, [paired, loadThreads]);

  // Subscribe to SSE.
  useEffect(() => {
    if (!paired) return;

    const unsub = api.onConnectionStatusChange(setConnectionStatus);
    const sub = api.subscribe(DEFAULT_PROJECT, (event: SSEEvent) => {
      if (event.type === "message") {
        // Reload threads to get updated order and previews.
        loadThreads();
      }
    });

    return () => {
      sub.close();
      unsub();
    };
  }, [paired, loadThreads]);

  const onRefresh = useCallback(async () => {
    setRefreshing(true);
    await loadThreads();
    setRefreshing(false);
  }, [loadThreads]);

  // Pairing check still in progress.
  if (paired === null) {
    return (
      <View style={styles.center}>
        <Text style={styles.muted}>Loading...</Text>
      </View>
    );
  }

  // Not paired — show first-launch flow.
  if (!paired) {
    return <PairingScreen onPaired={() => setPaired(true)} />;
  }

  return (
    <View style={styles.container}>
      <ConnectionStatusBar status={connectionStatus} />
      <FlatList
        data={threads}
        keyExtractor={(item) => item.thread_short}
        renderItem={({ item }) => (
          <ThreadRow
            thread={item}
            onPress={() =>
              router.push({
                pathname: "/thread/[id]",
                params: { id: item.thread_short, project: DEFAULT_PROJECT },
              })
            }
          />
        )}
        refreshControl={
          <RefreshControl refreshing={refreshing} onRefresh={onRefresh} />
        }
        ListEmptyComponent={
          <View style={styles.center}>
            <Text style={styles.muted}>No messages yet</Text>
          </View>
        }
        contentContainerStyle={threads.length === 0 ? styles.emptyList : undefined}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: "#ffffff" },
  center: { flex: 1, justifyContent: "center", alignItems: "center" },
  muted: { fontSize: 16, color: "#888888" },
  emptyList: { flex: 1 },
});
```

- [ ] **Step 2: Create the Thread Detail screen stub**

Create `app/(screens)/thread/[id].tsx`:

```tsx
import { useCallback, useEffect, useRef, useState } from "react";
import { FlatList, StyleSheet, View } from "react-native";
import { useLocalSearchParams } from "expo-router";

import { useApi } from "../../_layout";
import { DEFAULT_PROJECT, Message, SSEEvent } from "../../lib/types";
import { MessageBubble } from "../../components/MessageBubble";
import { ReplyComposer } from "../../components/ReplyComposer";

export default function ThreadDetailScreen() {
  const api = useApi();
  const { id, project = DEFAULT_PROJECT } = useLocalSearchParams<{
    id: string;
    project?: string;
  }>();
  const [messages, setMessages] = useState<Message[]>([]);
  const [replyText, setReplyText] = useState("");
  const [sending, setSending] = useState(false);
  const listRef = useRef<FlatList<Message>>(null);
  // Device peer ID for determining message alignment.
  const [selfPeerId, setSelfPeerId] = useState("");

  // Load thread messages.
  const loadMessages = useCallback(async () => {
    if (!id) return;
    try {
      const data = await api.getThread(project, id);
      setMessages(data);
    } catch {
      // Show empty state on error.
    }
  }, [id, project]);

  useEffect(() => {
    loadMessages();
  }, [loadMessages]);

  // Get self peer ID for alignment.
  useEffect(() => {
    api
      .getStatus()
      .then((s) => setSelfPeerId(s.peer_id))
      .catch(() => {});
  }, []);

  // Subscribe to new messages in this thread.
  useEffect(() => {
    const sub = api.subscribe(project, (event: SSEEvent) => {
      if (event.type === "message" && event.data.thread.includes(id)) {
        setMessages((prev) => [...prev, event.data]);
      }
    });
    return () => sub.close();
  }, [id, project]);

  // Scroll to bottom when messages change.
  useEffect(() => {
    if (messages.length > 0) {
      setTimeout(() => listRef.current?.scrollToEnd({ animated: true }), 100);
    }
  }, [messages.length]);

  const handleSend = useCallback(async () => {
    if (!replyText.trim() || sending) return;
    setSending(true);
    try {
      await api.sendMessage({
        project,
        body: replyText.trim(),
        thread: id,
      });
      setReplyText("");
    } catch {
      // TODO: show error toast
    } finally {
      setSending(false);
    }
  }, [replyText, sending, project, id]);

  const handleAction = useCallback(
    async (actionId: string) => {
      try {
        await api.sendMessage({
          project,
          body: actionId,
          thread: id,
        });
        // Reload to see the action response.
        loadMessages();
      } catch {
        // TODO: show error toast
      }
    },
    [project, id, loadMessages],
  );

  return (
    <View style={styles.container}>
      <FlatList
        ref={listRef}
        data={messages}
        keyExtractor={(item) => item.id}
        renderItem={({ item }) => (
          <MessageBubble
            message={item}
            isSelf={item.from === selfPeerId}
            onAction={handleAction}
          />
        )}
        contentContainerStyle={styles.list}
      />
      <ReplyComposer
        value={replyText}
        onChangeText={setReplyText}
        onSend={handleSend}
        disabled={sending}
      />
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: "#ffffff" },
  list: { paddingHorizontal: 16, paddingVertical: 8 },
});
```

- [ ] **Step 3: Create the Settings screen stub**

Create `app/(screens)/settings.tsx`:

```tsx
import { useCallback, useEffect, useState } from "react";
import {
  Alert,
  StyleSheet,
  Text,
  TouchableOpacity,
  View,
} from "react-native";

import { useApi } from "../_layout";
import { ConnectionStatus, StatusResponse } from "../lib/types";
import { formatConnectionStatus } from "../components/ConnectionStatus";

export default function SettingsScreen() {
  const api = useApi();
  const [status, setStatus] = useState<StatusResponse | null>(null);
  const [connectionStatus, setConnectionStatus] =
    useState<ConnectionStatus>("disconnected");

  useEffect(() => {
    const unsub = api.onConnectionStatusChange(setConnectionStatus);
    return unsub;
  }, []);

  const loadStatus = useCallback(async () => {
    try {
      const s = await api.getStatus();
      setStatus(s);
    } catch {
      setStatus(null);
    }
  }, []);

  useEffect(() => {
    loadStatus();
    // Poll status every 10 seconds.
    const interval = setInterval(loadStatus, 10000);
    return () => clearInterval(interval);
  }, [loadStatus]);

  const handleDisconnect = useCallback(() => {
    Alert.alert("Disconnect", "Disconnect from hub?", [
      { text: "Cancel", style: "cancel" },
      {
        text: "Disconnect",
        style: "destructive",
        onPress: async () => {
          try {
            await api.stop();
            setStatus(null);
          } catch {
            // Ignore — already disconnected.
          }
        },
      },
    ]);
  }, []);

  return (
    <View style={styles.container}>
      <View style={styles.section}>
        <Text style={styles.sectionTitle}>Connection</Text>
        <View style={styles.row}>
          <Text style={styles.label}>Status</Text>
          <Text
            style={[
              styles.value,
              connectionStatus === "connected"
                ? styles.connected
                : styles.disconnected,
            ]}
          >
            {formatConnectionStatus(connectionStatus)}
          </Text>
        </View>
        {status?.hub ? (
          <View style={styles.row}>
            <Text style={styles.label}>Hub</Text>
            <Text style={styles.value}>{status.hub}</Text>
          </View>
        ) : null}
        {status?.peer_id ? (
          <View style={styles.row}>
            <Text style={styles.label}>Peer ID</Text>
            <Text style={[styles.value, styles.mono]}>
              {status.peer_id.slice(0, 16)}...
            </Text>
          </View>
        ) : null}
      </View>

      {status?.hub ? (
        <View style={styles.section}>
          <TouchableOpacity
            style={styles.disconnectButton}
            onPress={handleDisconnect}
          >
            <Text style={styles.disconnectText}>Disconnect</Text>
          </TouchableOpacity>
        </View>
      ) : null}

      <View style={styles.section}>
        <Text style={styles.sectionTitle}>About</Text>
        <View style={styles.row}>
          <Text style={styles.label}>Version</Text>
          <Text style={styles.value}>0.1.0</Text>
        </View>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: { flex: 1, backgroundColor: "#ffffff", paddingTop: 16 },
  section: { marginBottom: 32, paddingHorizontal: 16 },
  sectionTitle: {
    fontSize: 13,
    fontWeight: "600",
    color: "#888888",
    textTransform: "uppercase",
    letterSpacing: 0.5,
    marginBottom: 12,
  },
  row: {
    flexDirection: "row",
    justifyContent: "space-between",
    alignItems: "center",
    paddingVertical: 12,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: "#e0e0e0",
  },
  label: { fontSize: 16, color: "#000000" },
  value: { fontSize: 16, color: "#444444" },
  mono: { fontFamily: "monospace", fontSize: 14 },
  connected: { color: "#34C759" },
  disconnected: { color: "#FF3B30" },
  disconnectButton: {
    paddingVertical: 12,
    alignItems: "center",
    borderRadius: 8,
    backgroundColor: "#FFF0F0",
  },
  disconnectText: { fontSize: 16, color: "#FF3B30", fontWeight: "600" },
});
```

- [ ] **Step 4: Verify TypeScript compiles**

Run: `cd edgesync-notify-app && npx tsc --noEmit`
Expected: No errors

- [ ] **Step 5: Commit**

```bash
git add app/\(screens\)/index.tsx app/\(screens\)/thread/\[id\].tsx app/\(screens\)/settings.tsx
git commit -m "feat: stub screens — inbox, thread detail, settings"
```

---

## Task 7: UI Components

**Files:**
- Create: `edgesync-notify-app/app/components/ConnectionStatus.tsx`
- Create: `edgesync-notify-app/app/components/ThreadRow.tsx`
- Create: `edgesync-notify-app/app/components/MessageBubble.tsx`
- Create: `edgesync-notify-app/app/components/ReplyComposer.tsx`
- Create: `edgesync-notify-app/app/components/PairingScreen.tsx`

Note: `ActionButton` and `PriorityBadge` are trivial (~15 lines each) and only used by a single parent component. They are co-located inside `MessageBubble.tsx` and `ThreadRow.tsx` respectively, rather than forced into separate files. Extract to separate files only if they gain a second consumer.

- [ ] **Step 1: Create ConnectionStatus**

Create `app/components/ConnectionStatus.tsx`:

```tsx
import { StyleSheet, Text, View } from "react-native";
import { ConnectionStatus } from "../lib/types";

// Shared utility — used by both InboxScreen and SettingsScreen.
export function formatConnectionStatus(status: ConnectionStatus): string {
  switch (status) {
    case "connected":
      return "Connected";
    case "connecting":
      return "Connecting...";
    case "disconnected":
      return "Disconnected";
  }
}

// Reusable status bar component for screens that show connection state.
interface ConnectionStatusBarProps {
  status: ConnectionStatus;
}

export function ConnectionStatusBar({ status }: ConnectionStatusBarProps) {
  return (
    <View style={styles.statusBar}>
      <Text style={styles.statusText}>{formatConnectionStatus(status)}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  statusBar: {
    paddingHorizontal: 16,
    paddingVertical: 8,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: "#e0e0e0",
  },
  statusText: { fontSize: 12, color: "#888888" },
});
```

- [ ] **Step 2: Create PriorityBadge (co-located in ThreadRow)**

PriorityBadge is ~15 lines and only used by ThreadRow, so it is defined in the same file rather than a separate component file (see Step 3). Shown here for reference:

```tsx
import { StyleSheet, Text, View } from "react-native";
import { Priority } from "../lib/types";

const BADGE_COLORS: Record<Priority, { bg: string; text: string }> = {
  urgent: { bg: "#FF3B30", text: "#FFFFFF" },
  action_required: { bg: "#FF9500", text: "#FFFFFF" },
  info: { bg: "#E0E0E0", text: "#666666" },
};

interface PriorityBadgeProps {
  priority: Priority;
}

export function PriorityBadge({ priority }: PriorityBadgeProps) {
  if (priority === "info") return null; // No badge for info — keep it clean.

  const colors = BADGE_COLORS[priority];
  const label = priority === "urgent" ? "Urgent" : "Action";

  return (
    <View style={[styles.badge, { backgroundColor: colors.bg }]}>
      <Text style={[styles.text, { color: colors.text }]}>{label}</Text>
    </View>
  );
}

const styles = StyleSheet.create({
  badge: {
    paddingHorizontal: 8,
    paddingVertical: 2,
    borderRadius: 4,
  },
  text: { fontSize: 11, fontWeight: "600" },
});
```

- [ ] **Step 3: Create ThreadRow (includes PriorityBadge)**

Create `app/components/ThreadRow.tsx`:

```tsx
import { StyleSheet, Text, TouchableOpacity, View } from "react-native";
import { Priority, ThreadSummary } from "../lib/types";

// Co-located — only used by ThreadRow. Extract if a second consumer appears.
const BADGE_COLORS: Record<Priority, { bg: string; text: string }> = {
  urgent: { bg: "#FF3B30", text: "#FFFFFF" },
  action_required: { bg: "#FF9500", text: "#FFFFFF" },
  info: { bg: "#E0E0E0", text: "#666666" },
};

function PriorityBadge({ priority }: { priority: Priority }) {
  if (priority === "info") return null;
  const colors = BADGE_COLORS[priority];
  const label = priority === "urgent" ? "Urgent" : "Action";
  return (
    <View style={[badgeStyles.badge, { backgroundColor: colors.bg }]}>
      <Text style={[badgeStyles.text, { color: colors.text }]}>{label}</Text>
    </View>
  );
}

const badgeStyles = StyleSheet.create({
  badge: { paddingHorizontal: 8, paddingVertical: 2, borderRadius: 4 },
  text: { fontSize: 11, fontWeight: "600" },
});

interface ThreadRowProps {
  thread: ThreadSummary;
  onPress: () => void;
}

export function ThreadRow({ thread, onPress }: ThreadRowProps) {
  const lastMsg = thread.last_message;
  const timeStr = formatRelativeTime(thread.last_activity);

  return (
    <TouchableOpacity style={styles.container} onPress={onPress}>
      <View style={styles.header}>
        <Text style={styles.threadId} numberOfLines={1}>
          {thread.thread_short}
        </Text>
        <View style={styles.headerRight}>
          <PriorityBadge priority={thread.priority} />
          <Text style={styles.time}>{timeStr}</Text>
        </View>
      </View>
      <Text style={styles.sender} numberOfLines={1}>
        {lastMsg.from_name}
      </Text>
      <Text style={styles.preview} numberOfLines={2}>
        {lastMsg.body}
      </Text>
    </TouchableOpacity>
  );
}

function formatRelativeTime(isoString: string): string {
  const date = new Date(isoString);
  const now = new Date();
  const diffMs = now.getTime() - date.getTime();
  const diffMin = Math.floor(diffMs / 60000);

  if (diffMin < 1) return "now";
  if (diffMin < 60) return `${diffMin}m`;
  const diffHr = Math.floor(diffMin / 60);
  if (diffHr < 24) return `${diffHr}h`;
  const diffDay = Math.floor(diffHr / 24);
  return `${diffDay}d`;
}

const styles = StyleSheet.create({
  container: {
    paddingHorizontal: 16,
    paddingVertical: 14,
    borderBottomWidth: StyleSheet.hairlineWidth,
    borderBottomColor: "#e0e0e0",
  },
  header: {
    flexDirection: "row",
    justifyContent: "space-between",
    alignItems: "center",
    marginBottom: 4,
  },
  headerRight: { flexDirection: "row", alignItems: "center", gap: 8 },
  threadId: {
    fontSize: 14,
    fontWeight: "600",
    color: "#000000",
    fontFamily: "monospace",
    flex: 1,
    marginRight: 8,
  },
  time: { fontSize: 13, color: "#888888" },
  sender: { fontSize: 14, color: "#444444", marginBottom: 2 },
  preview: { fontSize: 14, color: "#888888", lineHeight: 20 },
});
```

- [ ] **Step 4: Create MessageBubble (includes ActionButton)**

ActionButton is ~15 lines and only used by MessageBubble, so it is co-located here. Extract if a second consumer appears.

Create `app/components/MessageBubble.tsx`:

```tsx
import { StyleSheet, Text, TouchableOpacity, View } from "react-native";
import { Message } from "../lib/types";

// Co-located — only used by MessageBubble. Extract if a second consumer appears.
function ActionButton({ label, onPress }: { label: string; onPress: () => void }) {
  return (
    <TouchableOpacity style={actionStyles.button} onPress={onPress}>
      <Text style={actionStyles.label}>{label}</Text>
    </TouchableOpacity>
  );
}

const actionStyles = StyleSheet.create({
  button: {
    paddingHorizontal: 16,
    paddingVertical: 8,
    borderRadius: 8,
    borderWidth: 1,
    borderColor: "#007AFF",
    marginRight: 8,
    marginTop: 8,
  },
  label: { fontSize: 14, fontWeight: "500", color: "#007AFF" },
});

interface MessageBubbleProps {
  message: Message;
  isSelf: boolean;
  onAction: (actionId: string) => void;
}

export function MessageBubble({ message, isSelf, onAction }: MessageBubbleProps) {
  const timeStr = new Date(message.timestamp).toLocaleTimeString([], {
    hour: "2-digit",
    minute: "2-digit",
  });

  return (
    <View
      style={[
        styles.container,
        isSelf ? styles.selfContainer : styles.otherContainer,
      ]}
    >
      <View
        style={[styles.bubble, isSelf ? styles.selfBubble : styles.otherBubble]}
      >
        {!isSelf && (
          <Text style={styles.senderName}>{message.from_name}</Text>
        )}
        <Text style={[styles.body, isSelf ? styles.selfBody : styles.otherBody]}>
          {message.body}
        </Text>
        <Text
          style={[styles.time, isSelf ? styles.selfTime : styles.otherTime]}
        >
          {timeStr}
        </Text>
      </View>
      {message.actions && message.actions.length > 0 && (
        <View style={styles.actions}>
          {message.actions.map((action) => (
            <ActionButton
              key={action.id}
              label={action.label}
              onPress={() => onAction(action.id)}
            />
          ))}
        </View>
      )}
    </View>
  );
}

const styles = StyleSheet.create({
  container: { marginBottom: 12, maxWidth: "80%" },
  selfContainer: { alignSelf: "flex-end" },
  otherContainer: { alignSelf: "flex-start" },
  bubble: { padding: 12, borderRadius: 16 },
  selfBubble: {
    backgroundColor: "#007AFF",
    borderBottomRightRadius: 4,
  },
  otherBubble: {
    backgroundColor: "#F0F0F0",
    borderBottomLeftRadius: 4,
  },
  senderName: {
    fontSize: 12,
    fontWeight: "600",
    color: "#444444",
    marginBottom: 4,
  },
  body: { fontSize: 16, lineHeight: 22 },
  selfBody: { color: "#FFFFFF" },
  otherBody: { color: "#000000" },
  time: { fontSize: 11, marginTop: 4 },
  selfTime: { color: "rgba(255,255,255,0.7)", textAlign: "right" },
  otherTime: { color: "#888888" },
  actions: {
    flexDirection: "row",
    flexWrap: "wrap",
    marginTop: 4,
  },
});
```

- [ ] **Step 5: Create ReplyComposer (unchanged)**

Create `app/components/ReplyComposer.tsx`:

```tsx
import {
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from "react-native";

interface ReplyComposerProps {
  value: string;
  onChangeText: (text: string) => void;
  onSend: () => void;
  disabled?: boolean;
}

export function ReplyComposer({
  value,
  onChangeText,
  onSend,
  disabled,
}: ReplyComposerProps) {
  const canSend = value.trim().length > 0 && !disabled;

  return (
    <View style={styles.container}>
      <TextInput
        style={styles.input}
        value={value}
        onChangeText={onChangeText}
        placeholder="Reply..."
        placeholderTextColor="#AAAAAA"
        multiline
        maxLength={4000}
        editable={!disabled}
      />
      <TouchableOpacity
        style={[styles.sendButton, !canSend && styles.sendButtonDisabled]}
        onPress={onSend}
        disabled={!canSend}
      >
        <Text
          style={[styles.sendText, !canSend && styles.sendTextDisabled]}
        >
          Send
        </Text>
      </TouchableOpacity>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flexDirection: "row",
    alignItems: "flex-end",
    paddingHorizontal: 12,
    paddingVertical: 8,
    borderTopWidth: StyleSheet.hairlineWidth,
    borderTopColor: "#e0e0e0",
    backgroundColor: "#ffffff",
  },
  input: {
    flex: 1,
    fontSize: 16,
    lineHeight: 22,
    maxHeight: 100,
    paddingHorizontal: 12,
    paddingVertical: 8,
    borderRadius: 20,
    backgroundColor: "#F0F0F0",
    marginRight: 8,
  },
  sendButton: {
    paddingHorizontal: 16,
    paddingVertical: 8,
    borderRadius: 20,
    backgroundColor: "#007AFF",
  },
  sendButtonDisabled: { backgroundColor: "#E0E0E0" },
  sendText: { fontSize: 16, fontWeight: "600", color: "#FFFFFF" },
  sendTextDisabled: { color: "#AAAAAA" },
});
```

- [ ] **Step 6: Create PairingScreen**

Create `app/components/PairingScreen.tsx`:

```tsx
import { useCallback, useState } from "react";
import {
  Alert,
  StyleSheet,
  Text,
  TextInput,
  TouchableOpacity,
  View,
} from "react-native";

import { useApi } from "../_layout";
import { ApiError } from "../lib/api";

interface PairingScreenProps {
  onPaired: () => void;
}

export function PairingScreen({ onPaired }: PairingScreenProps) {
  const api = useApi();
  const [token, setToken] = useState("");
  const [pairing, setPairing] = useState(false);

  const handlePair = useCallback(async () => {
    const trimmed = token.trim().toUpperCase();
    if (!trimmed) return;

    setPairing(true);
    try {
      await api.pair({ token: trimmed });
      onPaired();
    } catch (err) {
      const message =
        err instanceof ApiError ? err.message : "Pairing failed";
      Alert.alert("Pairing Failed", message);
    } finally {
      setPairing(false);
    }
  }, [token, onPaired]);

  return (
    <View style={styles.container}>
      <View style={styles.content}>
        <Text style={styles.title}>Pair with Hub</Text>
        <Text style={styles.instructions}>
          On your hub, run:
        </Text>
        <View style={styles.codeBlock}>
          <Text style={styles.code}>
            edgesync notify pair --name "my-device"
          </Text>
        </View>
        <Text style={styles.instructions}>
          Then enter the pairing token below.
        </Text>

        <TextInput
          style={styles.tokenInput}
          value={token}
          onChangeText={setToken}
          placeholder="AXKF-9M2P-VR3T"
          placeholderTextColor="#AAAAAA"
          autoCapitalize="characters"
          autoCorrect={false}
          maxLength={14} // 12 chars + 2 dashes
          editable={!pairing}
        />

        <TouchableOpacity
          style={[
            styles.pairButton,
            (!token.trim() || pairing) && styles.pairButtonDisabled,
          ]}
          onPress={handlePair}
          disabled={!token.trim() || pairing}
        >
          <Text style={styles.pairButtonText}>
            {pairing ? "Pairing..." : "Pair"}
          </Text>
        </TouchableOpacity>
      </View>
    </View>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: "#ffffff",
    justifyContent: "center",
  },
  content: { paddingHorizontal: 32 },
  title: {
    fontSize: 28,
    fontWeight: "700",
    color: "#000000",
    textAlign: "center",
    marginBottom: 24,
  },
  instructions: {
    fontSize: 16,
    color: "#444444",
    textAlign: "center",
    marginBottom: 12,
    lineHeight: 22,
  },
  codeBlock: {
    backgroundColor: "#F5F5F5",
    borderRadius: 8,
    padding: 12,
    marginBottom: 16,
  },
  code: {
    fontFamily: "monospace",
    fontSize: 14,
    color: "#000000",
    textAlign: "center",
  },
  tokenInput: {
    fontSize: 24,
    fontFamily: "monospace",
    textAlign: "center",
    letterSpacing: 4,
    paddingVertical: 16,
    borderBottomWidth: 2,
    borderBottomColor: "#007AFF",
    marginBottom: 24,
    marginTop: 16,
  },
  pairButton: {
    backgroundColor: "#007AFF",
    paddingVertical: 14,
    borderRadius: 12,
    alignItems: "center",
  },
  pairButtonDisabled: { backgroundColor: "#E0E0E0" },
  pairButtonText: { fontSize: 18, fontWeight: "600", color: "#FFFFFF" },
});
```

- [ ] **Step 7: Verify TypeScript compiles**

Run: `cd edgesync-notify-app && npx tsc --noEmit`
Expected: No errors

- [ ] **Step 8: Commit**

```bash
git add app/components/
git commit -m "feat: UI components — connection status, thread row, message bubble, reply composer, pairing screen"
```

---

## Task 8: Swift Native Module

**Files:**
- Create: `edgesync-notify-app/ios/NotifyBridge.swift`

This native module calls the gomobile-generated `NotifyBridge.xcframework` and exposes the server port to React Native as a constant.

- [ ] **Step 1: Create the native module**

Create `ios/NotifyBridge.swift`:

```swift
import Foundation
import ExpoModulesCore
import NotifyBridge  // gomobile-generated xcframework

public class NotifyBridgeModule: Module {
  private var serverPort: Int = 0
  private var startError: String? = nil

  public func definition() -> ModuleDefinition {
    Name("NotifyBridge")

    // Called on app launch — starts the Go HTTP server.
    OnCreate {
      let dataDir = self.appDataDirectory()
      self.serverPort = Int(NotifyappStart(dataDir))
      if self.serverPort == 0 {
        // Read the error file written by Go's writeError() for a meaningful message.
        let errorFile = (dataDir as NSString).appendingPathComponent("server-error")
        let errorMsg = (try? String(contentsOfFile: errorFile, encoding: .utf8)) ?? "unknown error"
        self.startError = errorMsg
        NSLog("NotifyBridge: Failed to start Go server: \(errorMsg)")
      } else {
        self.startError = nil
        NSLog("NotifyBridge: Go server started on port \(self.serverPort)")
      }
    }

    // Called on app termination — stops the Go server.
    OnDestroy {
      NotifyappStop()
      NSLog("NotifyBridge: Go server stopped")
    }

    // Expose port and startup error to JavaScript as constants.
    Constants {
      return [
        "port": self.serverPort,
        "startError": self.startError as Any,
      ]
    }

    // Expose functions callable from JS.
    Function("getPort") {
      return self.serverPort
    }

    Function("isRunning") {
      return NotifyappIsRunning()
    }

    Function("stop") {
      NotifyappStop()
      self.serverPort = 0
    }

    Function("restart") { () -> Int in
      NotifyappStop()
      let dataDir = self.appDataDirectory()
      self.serverPort = Int(NotifyappStart(dataDir))
      return self.serverPort
    }
  }

  private func appDataDirectory() -> String {
    let paths = FileManager.default.urls(for: .documentDirectory, in: .userDomainMask)
    let docsDir = paths[0].appendingPathComponent("edgesync-notify")
    try? FileManager.default.createDirectory(at: docsDir, withIntermediateDirectories: true)
    return docsDir.path
  }
}
```

- [ ] **Step 2: Create the expo-module config**

Create `ios/NotifyBridge/expo-module.config.json`:

```json
{
  "platforms": ["ios"],
  "ios": {
    "modules": ["NotifyBridgeModule"]
  }
}
```

Note: The exact integration depends on `npx expo prebuild` generating the iOS project. The xcframework must be linked in the Xcode project. After prebuild:
1. Open `ios/EdgeSyncNotify.xcworkspace` in Xcode.
2. Drag `ios/NotifyBridge.xcframework` into the project.
3. Ensure it appears under "Frameworks, Libraries, and Embedded Content" with "Embed & Sign".

- [ ] **Step 3: Wire the port into the API client**

The port from the native module must be passed to `createApi(port)` at app startup, exposed to screens via React context. Replace `app/_layout.tsx`:

Replace the content of `app/_layout.tsx`:

```tsx
import { createContext, useContext, useMemo } from "react";
import { Stack } from "expo-router";
import { Text, View } from "react-native";
import { requireNativeModule } from "expo-modules-core";

import { Api, createApi } from "./lib/api";

// Get port and startup error from native module constants (set during app launch by Go server).
const NotifyBridge = requireNativeModule("NotifyBridge");
const port: number = NotifyBridge.port ?? 0;
const startError: string | null = NotifyBridge.startError ?? null;

// React context so screens can access the API instance.
const ApiContext = createContext<Api | null>(null);

export function useApi(): Api {
  const api = useContext(ApiContext);
  if (!api) throw new Error("useApi() called but Go server failed to start (port=0)");
  return api;
}

export default function RootLayout() {
  const api = useMemo(() => (port > 0 ? createApi(port) : null), []);

  if (!api) {
    return (
      <View style={{ flex: 1, justifyContent: "center", alignItems: "center" }}>
        <Text style={{ color: "#FF3B30" }}>
          Go server failed to start{startError ? `: ${startError}` : ""}
        </Text>
      </View>
    );
  }

  return (
    <ApiContext.Provider value={api}>
      <Stack
        screenOptions={{
          headerStyle: { backgroundColor: "#ffffff" },
          headerTintColor: "#000000",
          headerTitleStyle: { fontWeight: "600" },
          contentStyle: { backgroundColor: "#ffffff" },
        }}
      >
        <Stack.Screen name="(screens)" options={{ headerShown: false }} />
      </Stack>
    </ApiContext.Provider>
  );
}
```

Screens access the API via `useApi()` from `../_layout`. This makes the port dependency explicit at the type level: you cannot call API methods without a valid `Api` instance, and the instance cannot exist without a port.

- [ ] **Step 4: Verify TypeScript compiles**

Run: `cd edgesync-notify-app && npx tsc --noEmit`
Expected: No errors (the native module import may need a type declaration — if so, create `app/types/expo-modules.d.ts`):

```ts
declare module "expo-modules-core" {
  export function requireNativeModule(name: string): any;
}
```

- [ ] **Step 5: Commit**

```bash
git add ios/NotifyBridge.swift ios/NotifyBridge/ app/_layout.tsx
git commit -m "feat: Swift native module — bridges Go framework to React Native"
```

---

## Task 9: Integration Verification

This task verifies the full chain: build script, Expo prebuild, and TypeScript compilation.

- [ ] **Step 1: Build the Go framework**

Run:
```bash
cd edgesync-notify-app && ./scripts/build-go.sh ios
```
Expected: `ios/NotifyBridge.xcframework/` created with no errors.

- [ ] **Step 2: Run Expo prebuild**

Run:
```bash
cd edgesync-notify-app && npx expo prebuild --platform ios --clean
```
Expected: `ios/` directory populated with Xcode project files.

- [ ] **Step 3: Verify TypeScript compiles**

Run:
```bash
cd edgesync-notify-app && npx tsc --noEmit
```
Expected: No errors.

- [ ] **Step 4: Verify Go tests pass**

Run:
```bash
cd edgesync-notify-app && go test ./go/ -v -count=1
```
Expected: All lifecycle tests pass.

- [ ] **Step 5: Run on iOS Simulator (manual)**

Run:
```bash
cd edgesync-notify-app && npx expo run:ios
```

Expected: App launches in Simulator. If hub is not configured, the PairingScreen appears with instructions to run `edgesync notify pair`. If Go server fails to start (port = 0), the status bar shows "Disconnected".

- [ ] **Step 6: Commit final .gitignore updates**

Append to `.gitignore`:

```
# Expo
node_modules/
.expo/
ios/build/
ios/Pods/
macos/build/

# Go
ios/NotifyBridge.xcframework/
macos/NotifyBridge.xcframework/
```

```bash
git add .gitignore
git commit -m "chore: gitignore for Expo build artifacts and Go frameworks"
```
