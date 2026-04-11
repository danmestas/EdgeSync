# Notify Expo App — Phases 5-7: Integration, Notifications, and Sentry

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Wire the Go server (NotifyBridge.xcframework) into the Expo iOS app, add local notifications with background refresh, and integrate Sentry error tracking on both Go and React Native sides.

**Repo:** `edgesync-notify-app` (standalone GitHub repo)

**Specs:**
- `docs/superpowers/specs/2026-04-10-notify-expo-app-design.md` — app architecture, screens, Go server API
- `docs/superpowers/specs/2026-04-10-edgesync-notify-design.md` — backend design, message format, NATS subjects

**Prerequisites (Phases 1-4 already built):**
- Phase 1: Go HTTP server (`go/server.go`, `bridge.go`, `pair.go`, `main.go`) with gomobile exports (`Start`, `Stop`, `IsRunning`)
- Phase 2: Expo scaffold (`app/`, `app.json`, `package.json`) with screens (Inbox, Thread Detail, Settings)
- Phase 3: React Native components (`ThreadRow`, `MessageBubble`, `ActionButton`, `ReplyComposer`, `PriorityBadge`, `PairingScreen`)
- Phase 4: API layer (`app/lib/api.ts`) with SSE auto-reconnect, `app/lib/types.ts` with TypeScript types

**Key constraints:**
- `expo-notifications` for local notifications only — no APNs
- No notification preferences in app — iOS Focus modes handle mute/quiet hours
- `BGAppRefreshTask` gives ~30 seconds every 15+ minutes (iOS-controlled)
- SSE auto-reconnect already built in `api.ts`
- Go server queues missed messages internally (not in React Native)
- Sentry is additive — existing Honeycomb OTel in EdgeSync is separate
- No Claude co-author on commits

**App states:**

| State | NATS | Delivery | Notification |
|-------|------|----------|-------------|
| Foreground | Connected (SSE) | Real-time | None — UI updates |
| Background | Disconnected | Fossil sync via BGTask | Local notification |
| Killed | Dead | None | None until relaunch |

---

## File Structure

### Phase 5 — New Files

| File | Responsibility |
|------|---------------|
| `ios/NotifyBridge.swift` | Expo native module: calls Go `Start`/`Stop`, exposes port to React Native |
| `ios/NotifyBridgeModule.m` | Objective-C macro to register the Swift native module with React Native |
| `app/lib/native.ts` | Typed wrapper around `NativeModules.NotifyBridge` — reads port constant |
| `app/components/PairingFlow.tsx` | Orchestrates QR scan → token POST → connected → navigate to Inbox |
| `e2e/integration.test.ts` | End-to-end: Go server start → SSE → UI update → tap reply → Go sends |

### Phase 5 — Modified Files

| File | Change |
|------|--------|
| `ios/<AppName>/AppDelegate.mm` | Call `NotifyBridgeStart` in `didFinishLaunchingWithOptions`, `NotifyBridgeStop` in `applicationWillTerminate` |
| `app/lib/api.ts` | Import port from `native.ts` instead of hardcoded constant |
| `app/(screens)/index.tsx` | Show `PairingFlow` when not paired, `Inbox` when connected |
| `app/(screens)/settings.tsx` | Add "Disconnect" button that calls `POST /stop` + resets pairing state |
| `app.json` | Add `ios.infoPlist` entries for camera permission (QR scanning) |

### Phase 6 — New Files

| File | Responsibility |
|------|---------------|
| `ios/BackgroundSync.swift` | `BGAppRefreshTask` registration + handler — calls Go server for sync + fires local notifications |
| `app/lib/notifications.ts` | `expo-notifications` setup: permission request, channel config, fire-on-message helper |

### Phase 6 — Modified Files

| File | Change |
|------|--------|
| `ios/<AppName>/AppDelegate.mm` | Register `BGAppRefreshTask` identifier in `didFinishLaunchingWithOptions` |
| `go/server.go` | Add `GET /sync` endpoint (triggers Fossil sync, returns new message count), `GET /missed` endpoint (returns queued messages since last SSE disconnect), and `GET /notification-content?msg_id=X` endpoint (single source of truth for notification title/body/sound) |
| `go/bridge.go` | Track SSE client state: `connected` (SSE active) vs `disconnected` (no SSE client). Queue messages when disconnected. Replay on reconnect. |
| `app/lib/api.ts` | Add `syncNow()` and `getMissedMessages()` calls. SSE `onopen` handler calls `/missed` to replay queued messages. |
| `app/lib/types.ts` | Add `threadShortId(threadId: string): string` utility — single definition of thread ID format (`thread-` prefix + first 8 hex chars) |
| `app/(screens)/index.tsx` | Merge replayed messages into thread list on SSE reconnect (uses `threadShortId` from `types.ts`) |

### Phase 7 — New Files

| File | Responsibility |
|------|---------------|
| `go/sentry.go` | Sentry Go SDK init, panic recovery middleware, error capture helpers |
| `app/lib/sentry.ts` | `@sentry/react-native` init with navigation breadcrumbs and tags |

### Phase 7 — Modified Files

| File | Change |
|------|--------|
| `go/server.go` | Wrap HTTP handler with Sentry middleware (panic recovery + request tracing) |
| `go/bridge.go` | Capture NATS errors and sync failures to Sentry |
| `go/main.go` | Call `sentry.Init()` in `Start`, `sentry.Flush()` in `Stop` |
| `app/_layout.tsx` | Wrap app with `Sentry.wrap()`, init Sentry with DSN |
| `app.json` | Add `@sentry/react-native` plugin to Expo plugins array |
| `.github/workflows/ci.yml` | Add `SENTRY_DSN` secret to env |
| `package.json` | Add `@sentry/react-native` dependency |
| `go/go.mod` | Add `github.com/getsentry/sentry-go` dependency |

---

## Phase 5: Integration

### Task 1: Native Module — NotifyBridge.swift

**Files:**
- Create: `ios/NotifyBridge.swift`
- Create: `ios/NotifyBridgeModule.m`

- [ ] **Step 1: Write the Objective-C registration macro**

Create `ios/NotifyBridgeModule.m`:

```objc
#import <React/RCTBridgeModule.h>

@interface RCT_EXTERN_MODULE(NotifyBridge, NSObject)

RCT_EXTERN_METHOD(getPort:(RCTPromiseResolveBlock)resolve
                  rejecter:(RCTPromiseRejectBlock)reject)

@end
```

- [ ] **Step 2: Write the Swift native module**

Create `ios/NotifyBridge.swift`:

```swift
import Foundation
import NotifyBridgeXcframework  // gomobile-generated framework

@objc(NotifyBridge)
class NotifyBridge: NSObject {

  private static var port: Int = 0

  /// Called from AppDelegate on launch. Starts the Go HTTP server.
  @objc static func startServer() -> Int {
    let dataDir = NSSearchPathForDirectoriesInDomains(
      .documentDirectory, .userDomainMask, true
    ).first!
    let p = GoStart(dataDir)  // gomobile export: Start(dataDir string) int
    port = Int(p)
    return Int(p)
  }

  /// Called from AppDelegate on termination. Stops the Go server.
  @objc static func stopServer() {
    GoStop()  // gomobile export: Stop()
  }

  /// Exposes port to React Native via promise.
  @objc func getPort(
    _ resolve: @escaping RCTPromiseResolveBlock,
    rejecter reject: @escaping RCTPromiseRejectBlock
  ) {
    if NotifyBridge.port > 0 {
      resolve(NotifyBridge.port)
    } else {
      reject("NO_PORT", "Go server not started", nil)
    }
  }

  @objc static func requiresMainQueueSetup() -> Bool { return false }
}
```

- [ ] **Step 3: Verify the xcframework links**

Run:
```bash
cd /path/to/edgesync-notify-app/go
gomobile bind -target ios -o ../ios/NotifyBridge.xcframework .
```

Then:
```bash
cd /path/to/edgesync-notify-app
npx expo prebuild --clean
```

Verify: `ios/<AppName>.xcodeproj` references `NotifyBridge.xcframework` in "Frameworks, Libraries, and Embedded Content".

- [ ] **Step 4: Commit**

```bash
git add ios/NotifyBridge.swift ios/NotifyBridgeModule.m
git commit -m "feat: add NotifyBridge native module — Start/Stop/getPort"
```

---

### Task 2: AppDelegate Lifecycle Wiring

**Files:**
- Modify: `ios/<AppName>/AppDelegate.mm`

- [ ] **Step 1: Add Go server start/stop to AppDelegate**

Add to `ios/<AppName>/AppDelegate.mm`:

At the top, add the Swift bridging import:
```objc
#import "<AppName>-Swift.h"
```

In `didFinishLaunchingWithOptions:`, before `return YES`:
```objc
// Start the Go HTTP server. Port is stored in NotifyBridge for React Native access.
[NotifyBridge startServer];
```

In `applicationWillTerminate:` (add the method if it doesn't exist):
```objc
- (void)applicationWillTerminate:(UIApplication *)application {
  [NotifyBridge stopServer];
}
```

- [ ] **Step 2: Build and verify server starts**

Run:
```bash
cd /path/to/edgesync-notify-app
npx expo run:ios
```

Verify in Xcode console: the Go server logs its port on startup.

- [ ] **Step 3: Commit**

```bash
git add ios/
git commit -m "feat: wire Go server Start/Stop into AppDelegate lifecycle"
```

---

### Task 3: React Native Port Bridge

**Files:**
- Create: `app/lib/native.ts`
- Modify: `app/lib/api.ts`

- [ ] **Step 1: Create the typed native module wrapper**

Create `app/lib/native.ts`:

```typescript
import { NativeModules, Platform } from "react-native";

const { NotifyBridge } = NativeModules;

/**
 * Get the Go server port. Throws if the server hasn't started.
 * Caches the port after first call — it doesn't change during app lifetime.
 */
let cachedPort: number | null = null;

export async function getServerPort(): Promise<number> {
  if (cachedPort !== null) return cachedPort;

  if (Platform.OS !== "ios" && Platform.OS !== "macos") {
    throw new Error("NotifyBridge is only available on iOS/macOS");
  }

  const port: number = await NotifyBridge.getPort();
  cachedPort = port;
  return port;
}

export function getBaseURL(port: number): string {
  return `http://127.0.0.1:${port}`;
}
```

- [ ] **Step 2: Update api.ts to use native port**

In `app/lib/api.ts`, replace the hardcoded base URL with:

```typescript
import { getServerPort, getBaseURL } from "./native";

let baseURL: string | null = null;

async function ensureBaseURL(): Promise<string> {
  if (baseURL) return baseURL;
  const port = await getServerPort();
  baseURL = getBaseURL(port);
  return baseURL;
}
```

Update each fetch/SSE call to `await ensureBaseURL()` before constructing the URL.

- [ ] **Step 3: Verify fetch works end-to-end**

Run the app in Simulator. Open React Native debugger. Verify:
- `getServerPort()` returns a valid port
- `GET /status` returns `{"connected": false, ...}` (not yet paired)

- [ ] **Step 4: Commit**

```bash
git add app/lib/native.ts app/lib/api.ts
git commit -m "feat: bridge Go server port to React Native via native module"
```

---

### Task 4: Pairing Flow

**Files:**
- Create: `app/components/PairingFlow.tsx`
- Modify: `app/(screens)/index.tsx`
- Modify: `app/(screens)/settings.tsx`
- Modify: `app.json`

- [ ] **Step 1: Add camera permission for QR scanning**

In `app.json`, add to `expo.ios`:

```json
{
  "infoPlist": {
    "NSCameraUsageDescription": "Scan a pairing QR code to connect to your EdgeSync hub"
  }
}
```

Add `expo-camera` to plugins if not already present:

```json
{
  "plugins": ["expo-camera"]
}
```

- [ ] **Step 2: Build the PairingFlow component**

Create `app/components/PairingFlow.tsx`:

```tsx
import { useState } from "react";
import { View, Text, TextInput, TouchableOpacity, StyleSheet, Alert } from "react-native";
import { CameraView, useCameraPermissions } from "expo-camera";

type Props = {
  onPaired: () => void;
};

export function PairingFlow({ onPaired }: Props) {
  const [mode, setMode] = useState<"choose" | "qr" | "token">("choose");
  const [token, setToken] = useState("");
  const [pairing, setPairing] = useState(false);
  const [permission, requestPermission] = useCameraPermissions();

  async function submitToken(rawToken: string) {
    setPairing(true);
    try {
      const url = await ensureBaseURL();
      const res = await fetch(`${url}/pair`, {
        method: "POST",
        headers: { "Content-Type": "application/json" },
        body: JSON.stringify({ token: rawToken.trim() }),
      });
      if (!res.ok) {
        const err = await res.text();
        Alert.alert("Pairing Failed", err);
        return;
      }
      onPaired();
    } catch (e: any) {
      Alert.alert("Pairing Error", e.message);
    } finally {
      setPairing(false);
    }
  }

  function handleQRScan(data: string) {
    // QR encodes: edgesync-pair://v1/<hub-endpoint-id>/<nats-addr>/<secret>
    // The Go server's POST /pair accepts the full URI or just the token portion.
    submitToken(data);
  }

  if (mode === "qr") {
    if (!permission?.granted) {
      return (
        <View style={styles.container}>
          <Text style={styles.label}>Camera permission required to scan QR code</Text>
          <TouchableOpacity style={styles.button} onPress={requestPermission}>
            <Text style={styles.buttonText}>Grant Permission</Text>
          </TouchableOpacity>
        </View>
      );
    }
    return (
      <View style={styles.container}>
        <CameraView
          style={styles.camera}
          onBarcodeScanned={({ data }) => handleQRScan(data)}
          barcodeScannerSettings={{ barcodeTypes: ["qr"] }}
        />
        <TouchableOpacity style={styles.link} onPress={() => setMode("choose")}>
          <Text style={styles.linkText}>Back</Text>
        </TouchableOpacity>
      </View>
    );
  }

  if (mode === "token") {
    return (
      <View style={styles.container}>
        <Text style={styles.label}>Enter Pairing Token</Text>
        <Text style={styles.hint}>
          Run: edgesync notify pair --name "my-device"
        </Text>
        <TextInput
          style={styles.input}
          value={token}
          onChangeText={setToken}
          placeholder="AXKF-9M2P-VR3T"
          autoCapitalize="characters"
          autoCorrect={false}
        />
        <TouchableOpacity
          style={[styles.button, pairing && styles.disabled]}
          onPress={() => submitToken(token)}
          disabled={pairing || token.length === 0}
        >
          <Text style={styles.buttonText}>{pairing ? "Pairing..." : "Connect"}</Text>
        </TouchableOpacity>
        <TouchableOpacity style={styles.link} onPress={() => setMode("choose")}>
          <Text style={styles.linkText}>Back</Text>
        </TouchableOpacity>
      </View>
    );
  }

  // mode === "choose"
  return (
    <View style={styles.container}>
      <Text style={styles.title}>Connect to Hub</Text>
      <Text style={styles.hint}>
        On your hub, run:{"\n"}edgesync notify pair --name "my-device"
      </Text>
      <TouchableOpacity style={styles.button} onPress={() => setMode("qr")}>
        <Text style={styles.buttonText}>Scan QR Code</Text>
      </TouchableOpacity>
      <TouchableOpacity style={styles.buttonSecondary} onPress={() => setMode("token")}>
        <Text style={styles.buttonSecondaryText}>Enter Token</Text>
      </TouchableOpacity>
    </View>
  );
}
```

Styles are minimal placeholders — real styling comes later.

- [ ] **Step 3: Wire PairingFlow into Inbox screen**

In `app/(screens)/index.tsx`, add pairing gate:

```tsx
import { PairingFlow } from "../components/PairingFlow";

// Inside the component:
const [paired, setPaired] = useState(false);

// On mount, check GET /status — if connected or has hub config, skip pairing.
useEffect(() => {
  async function check() {
    const url = await ensureBaseURL();
    const res = await fetch(`${url}/status`);
    const data = await res.json();
    if (data.connected || data.hub) {
      setPaired(true);
    }
  }
  check();
}, []);

if (!paired) {
  return <PairingFlow onPaired={() => setPaired(true)} />;
}

// ... existing Inbox UI
```

- [ ] **Step 4: Add disconnect to Settings**

In `app/(screens)/settings.tsx`, add a "Disconnect" button that calls `POST /stop` and navigates back to the pairing screen:

```tsx
async function handleDisconnect() {
  const url = await ensureBaseURL();
  await fetch(`${url}/stop`, { method: "POST" });
  // Navigate to root which will show PairingFlow
  router.replace("/");
}
```

- [ ] **Step 5: Test pairing flow in Simulator**

1. Start the hub: `edgesync notify pair --name "sim-device"` — get the token
2. Run the app in Simulator
3. Choose "Enter Token", paste the token
4. Verify: `POST /pair` succeeds, screen transitions to Inbox
5. Verify: `GET /status` now shows `connected: true`

- [ ] **Step 6: Commit**

```bash
git add app/components/PairingFlow.tsx app/(screens)/index.tsx app/(screens)/settings.tsx app.json
git commit -m "feat: add device pairing flow — QR scan + token entry"
```

---

### Task 5: End-to-End Integration Test

**Files:**
- Create: `e2e/integration.test.ts`

- [ ] **Step 1: Write the integration test**

Create `e2e/integration.test.ts`. This is a manual test script (not automated CI — requires iOS Simulator and a running hub). Document the steps:

```typescript
/**
 * End-to-end integration test — run manually in Simulator.
 *
 * Prerequisites:
 *   1. Hub running: edgesync notify init && edgesync leaf start --notify
 *   2. Pairing token ready: edgesync notify pair --name "e2e-test"
 *   3. App built: npx expo run:ios
 *
 * Test sequence:
 *   1. App launches → Go server starts (check Xcode console for port log)
 *   2. Pairing screen shown → enter token → POST /pair → connected
 *   3. Send from hub: edgesync notify send --project test "Hello from hub" --priority urgent
 *   4. Verify: SSE event arrives → Inbox shows new thread → urgent badge visible
 *   5. Tap thread → Thread Detail opens → message bubble shows "Hello from hub"
 *   6. Type reply "Got it" → tap Send → POST /send succeeds
 *   7. On hub: edgesync notify watch --project test → verify reply appears
 *   8. Send action message: edgesync notify send --project test --thread <id> \
 *        "Continue?" --actions "Yes,No" --priority action_required
 *   9. Verify: action buttons render below message
 *  10. Tap "Yes" → verify POST /send with action_response: true
 *  11. On hub: watch output shows "action:yes"
 */
```

- [ ] **Step 2: Run the test manually and document results**

Run through all 11 steps. Note any issues in a comment at the bottom of the file.

- [ ] **Step 3: Commit**

```bash
git add e2e/integration.test.ts
git commit -m "test: add end-to-end integration test script for full message loop"
```

---

## Phase 6: Notifications + Background

### Task 6: Go Server — SSE Client Tracking + Message Queue

**Files:**
- Modify: `go/bridge.go`
- Modify: `go/server.go`

- [ ] **Step 1: Add SSE client state tracking to bridge.go**

In `go/bridge.go`, add:

```go
// sseState tracks whether an SSE client is connected.
type sseState struct {
	mu        sync.Mutex
	connected bool
	queue     []notify.Message // messages received while SSE disconnected
}

func (s *sseState) setConnected(v bool) {
	s.mu.Lock()
	defer s.mu.Unlock()
	s.connected = v
}

func (s *sseState) isConnected() bool {
	s.mu.Lock()
	defer s.mu.Unlock()
	return s.connected
}

// enqueue adds a message to the queue when SSE is disconnected.
func (s *sseState) enqueue(msg notify.Message) {
	s.mu.Lock()
	defer s.mu.Unlock()
	if !s.connected {
		s.queue = append(s.queue, msg)
	}
}

// drain returns all queued messages and clears the queue.
func (s *sseState) drain() []notify.Message {
	s.mu.Lock()
	defer s.mu.Unlock()
	msgs := s.queue
	s.queue = nil
	return msgs
}
```

Update the existing `Watch` goroutine (which reads from `svc.Watch()`) to call `sseState.enqueue()` when the SSE client is disconnected instead of dropping the message.

- [ ] **Step 2: Add /missed endpoint to server.go**

In `go/server.go`, register `GET /missed`:

```go
mux.HandleFunc("GET /missed", func(w http.ResponseWriter, r *http.Request) {
    msgs := sse.drain()
    w.Header().Set("Content-Type", "application/json")
    json.NewEncoder(w).Encode(msgs)
})
```

- [ ] **Step 3: Add /notification-content endpoint to server.go**

In `go/server.go`, register `GET /notification-content`. This is the **single source of truth** for notification content construction (priority-to-sound mapping, body truncation). Both the React Native foreground handler (`notifications.ts`) and the Swift background handler (`BackgroundSync.swift`) consume this endpoint.

```go
mux.HandleFunc("GET /notification-content", func(w http.ResponseWriter, r *http.Request) {
    msgID := r.URL.Query().Get("msg_id")
    if msgID == "" {
        http.Error(w, "msg_id required", http.StatusBadRequest)
        return
    }
    msg, ok := sse.findMessage(msgID)
    if !ok {
        http.Error(w, "message not found", http.StatusNotFound)
        return
    }
    body := msg.Body
    if len(body) > 200 {
        body = body[:200] + "..."
    }
    sound := msg.Priority == "urgent" || msg.Priority == "action_required"
    w.Header().Set("Content-Type", "application/json")
    json.NewEncoder(w).Encode(map[string]any{
        "title":    msg.FromName,
        "body":     body,
        "sound":    sound,
        "threadId": msg.Thread,
        "project":  msg.Project,
    })
})
```

> **Design note:** `notifications.ts` can also call this endpoint instead of using its local `buildNotificationContent()` function for full single-source-of-truth purity. The local TS function is kept as an optimization (avoids a round-trip for foreground notifications where the `Message` object is already in memory), but its logic must stay in sync with the Go endpoint. If this drift risk is unacceptable, remove `buildNotificationContent()` from `notifications.ts` and always call the Go endpoint.

- [ ] **Step 4: Add /sync endpoint to server.go**

In `go/server.go`, register `GET /sync`:

```go
mux.HandleFunc("GET /sync", func(w http.ResponseWriter, r *http.Request) {
    // Trigger a Fossil sync and return new message count.
    // svc.Repo().Sync() performs the sync.
    err := svc.Repo().Sync(r.Context(), syncTransport, libfossil.SyncOpts{})
    if err != nil {
        http.Error(w, err.Error(), http.StatusInternalServerError)
        return
    }
    // Count messages committed since last sync by reading the repo.
    // The BGTask handler uses this count to decide whether to fire a notification.
    json.NewEncoder(w).Encode(map[string]string{"status": "ok"})
})
```

- [ ] **Step 5: Update SSE handler to set connected state**

In the existing `/subscribe` handler, add:

```go
// Mark SSE as connected when client connects.
sse.setConnected(true)

// On disconnect (client closes connection):
defer sse.setConnected(false)

// Replay missed messages before streaming new ones.
for _, msg := range sse.drain() {
    // Write SSE event for each queued message.
    fmt.Fprintf(w, "event: message\ndata: %s\n\n", mustJSON(msg))
    flusher.Flush()
}
```

- [ ] **Step 6: Write tests for SSE state tracking**

In `go/bridge_test.go`, add:

```go
func TestSSEStateQueueAndDrain(t *testing.T) {
    state := &sseState{}

    // Not connected — messages should queue.
    msg1 := notify.NewMessage(notify.MessageOpts{
        Project: "test", From: "a", FromName: "a", Body: "hello",
    })
    state.enqueue(msg1)

    if len(state.drain()) != 1 {
        t.Fatal("expected 1 queued message")
    }
    if len(state.drain()) != 0 {
        t.Fatal("drain should clear the queue")
    }

    // When connected, enqueue should be a no-op.
    state.setConnected(true)
    state.enqueue(msg1)
    if len(state.drain()) != 0 {
        t.Fatal("should not queue when connected")
    }
}
```

- [ ] **Step 7: Run tests**

Run: `cd /path/to/edgesync-notify-app/go && go test ./... -v -count=1`
Expected: PASS

- [ ] **Step 8: Commit**

```bash
git add go/bridge.go go/server.go go/bridge_test.go
git commit -m "feat: track SSE client state and queue messages for replay on reconnect"
```

---

### Task 7: React Native — Replay Missed Messages on Reconnect

**Files:**
- Modify: `app/lib/api.ts`
- Modify: `app/(screens)/index.tsx`

- [ ] **Step 1: Add syncNow and getMissedMessages to api.ts**

In `app/lib/api.ts`:

```typescript
export async function syncNow(): Promise<void> {
  const url = await ensureBaseURL();
  const res = await fetch(`${url}/sync`);
  if (!res.ok) throw new Error(`sync failed: ${res.status}`);
}

export async function getMissedMessages(): Promise<Message[]> {
  const url = await ensureBaseURL();
  const res = await fetch(`${url}/missed`);
  if (!res.ok) return [];
  return res.json();
}
```

- [ ] **Step 2: Replay missed messages on SSE reconnect**

In the existing SSE setup in `api.ts`, add to the `onopen` handler:

```typescript
eventSource.onopen = async () => {
  setConnectionStatus("connected");
  // Replay any messages queued while SSE was disconnected.
  const missed = await getMissedMessages();
  for (const msg of missed) {
    onMessage(msg);
  }
};
```

- [ ] **Step 3: Merge replayed messages in Inbox**

In `app/lib/types.ts`, add the `threadShortId` utility alongside the type definitions. This documents the thread ID format (`thread-` prefix + first 8 hex chars) in one place:

```typescript
/**
 * Extract the short display ID from a full thread ID.
 * Thread IDs have the format "thread-<hex>"; this returns the first 8 hex chars.
 *
 * Example: "thread-a1b2c3d4e5f6" → "a1b2c3d4"
 */
export function threadShortId(threadId: string): string {
  const PREFIX = "thread-";
  const SHORT_LEN = 8;
  if (threadId.startsWith(PREFIX)) {
    return threadId.substring(PREFIX.length, PREFIX.length + SHORT_LEN);
  }
  return threadId.substring(0, SHORT_LEN);
}
```

In `app/(screens)/index.tsx`, import `threadShortId` and use it for deduplication. Ensure the `onMessage` callback merges into the existing thread list without duplicates (use message `id` for deduplication):

```typescript
import { threadShortId } from "../lib/types";

function handleIncomingMessage(msg: Message) {
  const shortId = threadShortId(msg.thread);
  setThreads((prev) => {
    // Check if thread already exists.
    const existing = prev.find((t) => t.threadShort === shortId);
    if (existing) {
      // Update the existing thread's last message.
      return prev.map((t) =>
        t.threadShort === shortId
          ? { ...t, lastMessage: msg, lastActivity: msg.timestamp }
          : t
      ).sort(threadSortComparator);
    }
    // New thread — add it.
    return [
      {
        threadShort: shortId,
        project: msg.project,
        lastActivity: msg.timestamp,
        messageCount: 1,
        lastMessage: msg,
        priority: msg.priority || "info",
      },
      ...prev,
    ].sort(threadSortComparator);
  });
}
```

- [ ] **Step 4: Commit**

```bash
git add app/lib/api.ts app/(screens)/index.tsx
git commit -m "feat: replay missed messages on SSE reconnect"
```

---

### Task 8: expo-notifications Setup

**Files:**
- Create: `app/lib/notifications.ts`
- Modify: `app/(screens)/index.tsx`

- [ ] **Step 1: Install expo-notifications**

Run:
```bash
cd /path/to/edgesync-notify-app
npx expo install expo-notifications
```

- [ ] **Step 2: Create notifications helper**

Create `app/lib/notifications.ts`:

```typescript
import * as Notifications from "expo-notifications";
import { Platform } from "react-native";
import type { Message, Priority } from "./types";

// Configure how notifications appear when the app is in the foreground.
// We don't show them in foreground — UI updates handle it.
Notifications.setNotificationHandler({
  handleNotification: async () => ({
    shouldShowAlert: false,
    shouldPlaySound: false,
    shouldSetBadge: false,
  }),
});

/**
 * Request notification permissions. Call once on first launch.
 * Returns true if granted.
 */
export async function requestPermissions(): Promise<boolean> {
  const { status: existing } = await Notifications.getPermissionsAsync();
  if (existing === "granted") return true;

  const { status } = await Notifications.requestPermissionsAsync();
  return status === "granted";
}

/**
 * Build notification content from a Message.
 *
 * This is the SINGLE place that maps priority → sound and truncates body.
 * Both the foreground TS handler and BackgroundSync.swift call this
 * (BackgroundSync calls the Go equivalent — see GET /notification-content).
 */
function buildNotificationContent(msg: Message) {
  return {
    title: msg.from_name,
    body: msg.body.length > 200 ? msg.body.substring(0, 200) + "..." : msg.body,
    data: { threadId: msg.thread, messageId: msg.id, project: msg.project },
    sound: msg.priority === "urgent" || msg.priority === "action_required",
  };
}

/**
 * Fire a local notification for a message received in the foreground layer.
 * Uses buildNotificationContent() for content construction.
 */
export async function notifyMessage(msg: Message): Promise<void> {
  const content = buildNotificationContent(msg);

  await Notifications.scheduleNotificationAsync({
    content,
    trigger: null, // Fire immediately.
  });
}

/**
 * Set up notification tap handler — navigates to the thread.
 * Call once in the root layout.
 */
export function setupNotificationResponseHandler(
  navigate: (threadId: string, project: string) => void
): Notifications.Subscription {
  return Notifications.addNotificationResponseReceivedListener((response) => {
    const data = response.notification.request.content.data;
    if (data?.threadId && data?.project) {
      navigate(data.threadId as string, data.project as string);
    }
  });
}
```

- [ ] **Step 3: Request permissions on first paired launch**

In `app/(screens)/index.tsx`, after pairing succeeds:

```typescript
import { requestPermissions } from "../lib/notifications";

// After setPaired(true):
useEffect(() => {
  if (paired) {
    requestPermissions();
  }
}, [paired]);
```

- [ ] **Step 4: Set up notification tap handler in root layout**

In `app/_layout.tsx`:

```typescript
import { setupNotificationResponseHandler } from "./lib/notifications";
import { useRouter } from "expo-router";

// Inside the layout component:
const router = useRouter();

useEffect(() => {
  const sub = setupNotificationResponseHandler((threadId, project) => {
    router.push(`/thread/${threadId}?project=${project}`);
  });
  return () => sub.remove();
}, []);
```

- [ ] **Step 5: Commit**

```bash
git add app/lib/notifications.ts app/(screens)/index.tsx app/_layout.tsx package.json
git commit -m "feat: add expo-notifications — permissions, local push, tap-to-navigate"
```

---

### Task 9: BGAppRefreshTask — Background Sync

**Files:**
- Create: `ios/BackgroundSync.swift`
- Modify: `ios/<AppName>/AppDelegate.mm`

- [ ] **Step 1: Register the BGTask identifier in Info.plist**

In `app.json`, add to `expo.ios.infoPlist`:

```json
{
  "BGTaskSchedulerPermittedIdentifiers": [
    "com.edgesync.notify.sync"
  ]
}
```

- [ ] **Step 2: Write the BackgroundSync handler**

Create `ios/BackgroundSync.swift`:

```swift
import UIKit
import BackgroundTasks

class BackgroundSync {

  static let taskIdentifier = "com.edgesync.notify.sync"

  /// Register the BGAppRefreshTask. Call once in didFinishLaunchingWithOptions.
  static func register() {
    BGTaskScheduler.shared.register(
      forTaskWithIdentifier: taskIdentifier,
      using: nil
    ) { task in
      handleSync(task: task as! BGAppRefreshTask)
    }
  }

  /// Schedule the next background refresh.
  static func scheduleNext() {
    let request = BGAppRefreshTaskRequest(identifier: taskIdentifier)
    request.earliestBeginDate = Date(timeIntervalSinceNow: 15 * 60) // 15 minutes
    do {
      try BGTaskScheduler.shared.submit(request)
    } catch {
      print("BackgroundSync: failed to schedule: \(error)")
    }
  }

  /// Handle a background refresh task.
  /// Calls GET /sync on the Go server, then GET /threads to check for new messages.
  /// Fires local notifications for any new messages since last check.
  private static func handleSync(task: BGAppRefreshTask) {
    // Schedule the next task before doing work.
    scheduleNext()

    let port = NotifyBridge.startServer() // No-op if already running, returns cached port.
    let baseURL = "http://127.0.0.1:\(port)"

    task.expirationHandler = {
      // iOS is reclaiming time — stop gracefully.
    }

    let url = URL(string: "\(baseURL)/sync")!
    let syncTask = URLSession.shared.dataTask(with: url) { _, response, error in
      guard error == nil,
            let httpResponse = response as? HTTPURLResponse,
            httpResponse.statusCode == 200 else {
        task.setTaskCompleted(success: false)
        return
      }

      // Fetch missed messages and fire notifications.
      // We use GET /missed to get message IDs, then GET /notification-content
      // for each message. The Go server owns the notification content logic
      // (priority-to-sound mapping, body truncation) — no duplication in Swift.
      let missedURL = URL(string: "\(baseURL)/missed")!
      let missedTask = URLSession.shared.dataTask(with: missedURL) { data, _, _ in
        defer { task.setTaskCompleted(success: true) }

        guard let data = data,
              let messages = try? JSONDecoder().decode([MissedMessage].self, from: data),
              !messages.isEmpty else {
          return
        }

        // Fire a local notification for each message using Go-provided content.
        for msg in messages {
          let contentURL = URL(string: "\(baseURL)/notification-content?msg_id=\(msg.id)")!
          // Synchronous fetch is acceptable here — BGTask runs off main thread.
          guard let contentData = try? Data(contentsOf: contentURL),
                let nc = try? JSONDecoder().decode(NotificationContent.self, from: contentData) else {
            continue
          }

          let content = UNMutableNotificationContent()
          content.title = nc.title
          content.body = nc.body
          content.sound = nc.sound ? .default : nil
          content.userInfo = [
            "threadId": nc.threadId,
            "messageId": msg.id,
            "project": nc.project,
          ]

          let request = UNNotificationRequest(
            identifier: msg.id,
            content: content,
            trigger: nil // Fire immediately.
          )
          UNUserNotificationCenter.current().add(request)
        }
      }
      missedTask.resume()
    }
    syncTask.resume()
  }
}

/// Decode /missed response — only need the id to call /notification-content.
/// Uses the JSON returned by Go's notify.Message directly (Codable decode),
/// not a parallel struct mirroring Go fields.
private struct MissedMessage: Decodable {
  let id: String
}

/// Decode GET /notification-content response.
/// The Go server owns priority-to-sound mapping and body truncation —
/// this struct just carries the pre-built values.
private struct NotificationContent: Decodable {
  let title: String
  let body: String
  let sound: Bool
  let threadId: String
  let project: String
}
```

- [ ] **Step 3: Wire BackgroundSync into AppDelegate**

In `ios/<AppName>/AppDelegate.mm`, in `didFinishLaunchingWithOptions:`:

```objc
[BackgroundSync register];
[BackgroundSync scheduleNext];
```

- [ ] **Step 4: Test BGTask in Simulator**

Use Xcode's debug command to trigger the BGTask:
```
e -l objc -- (void)[[BGTaskScheduler sharedScheduler] _simulateLaunchForTaskWithIdentifier:@"com.edgesync.notify.sync"]
```

Verify: the Go server syncs and local notification fires for any pending messages.

- [ ] **Step 5: Commit**

```bash
git add ios/BackgroundSync.swift ios/<AppName>/AppDelegate.mm app.json
git commit -m "feat: add BGAppRefreshTask — periodic background sync with local notifications"
```

---

## Phase 7: Sentry

### Task 10: Sentry Go SDK — Error Tracking in Go Server

**Files:**
- Create: `go/sentry.go`
- Modify: `go/main.go`
- Modify: `go/server.go`
- Modify: `go/bridge.go`
- Modify: `go/go.mod`

- [ ] **Step 1: Add sentry-go dependency**

Run:
```bash
cd /path/to/edgesync-notify-app/go
go get github.com/getsentry/sentry-go
```

- [ ] **Step 2: Create sentry.go with init and middleware**

Create `go/sentry.go`:

```go
package notifyapp

import (
	"fmt"
	"log/slog"
	"net/http"
	"time"

	"github.com/getsentry/sentry-go"
)

// InitSentry initializes the Sentry SDK. DSN comes from environment (SENTRY_DSN).
// Returns nil error if DSN is empty — Sentry is optional.
func InitSentry(dsn string, tags map[string]string) error {
	if dsn == "" {
		slog.Info("sentry: DSN not set, error tracking disabled")
		return nil
	}

	err := sentry.Init(sentry.ClientOptions{
		Dsn:              dsn,
		TracesSampleRate: 0.1,
		Environment:      "production",
	})
	if err != nil {
		return fmt.Errorf("sentry init: %w", err)
	}

	// Set global tags.
	for k, v := range tags {
		sentry.ConfigureScope(func(scope *sentry.Scope) {
			scope.SetTag(k, v)
		})
	}

	return nil
}

// FlushSentry drains the Sentry event queue. Call in Stop().
func FlushSentry() {
	sentry.Flush(2 * time.Second)
}

// SentryMiddleware wraps an http.Handler with panic recovery and request context.
func SentryMiddleware(next http.Handler) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		hub := sentry.GetHubFromContext(r.Context())
		if hub == nil {
			hub = sentry.CurrentHub().Clone()
		}
		hub.Scope().SetRequest(r)

		defer func() {
			if err := recover(); err != nil {
				hub.RecoverWithContext(r.Context(), err)
				http.Error(w, "Internal Server Error", http.StatusInternalServerError)
			}
		}()

		next.ServeHTTP(w, r)
	})
}

// CaptureError sends an error to Sentry with optional tags.
func CaptureError(err error, tags map[string]string) {
	if err == nil {
		return
	}
	hub := sentry.CurrentHub().Clone()
	for k, v := range tags {
		hub.Scope().SetTag(k, v)
	}
	hub.CaptureException(err)
}
```

- [ ] **Step 3: Wire Sentry init into main.go Start()**

In `go/main.go`, in the `Start` function:

```go
// Read SENTRY_DSN from environment or from a config file in dataDir.
dsn := os.Getenv("SENTRY_DSN")
if dsn == "" {
    // Check for DSN file (written by app settings or bundled).
    dsnBytes, _ := os.ReadFile(filepath.Join(dataDir, "sentry-dsn"))
    dsn = strings.TrimSpace(string(dsnBytes))
}

InitSentry(dsn, map[string]string{
    "component": "notify-app",
    "device":    deviceName,
})
```

In the `Stop` function:

```go
FlushSentry()
```

- [ ] **Step 4: Wrap HTTP handler with Sentry middleware**

In `go/server.go`, wrap the mux:

```go
// Before:
// server := &http.Server{Addr: addr, Handler: mux}

// After:
server := &http.Server{Addr: addr, Handler: SentryMiddleware(mux)}
```

- [ ] **Step 5: Capture NATS and sync errors in bridge.go**

In `go/bridge.go`, in error paths:

```go
// In the Watch goroutine error handling:
if err := svc.Repo().Sync(ctx, transport, libfossil.SyncOpts{}); err != nil {
    CaptureError(err, map[string]string{"operation": "fossil_sync"})
    slog.Error("sync failed", "error", err)
}

// In NATS connection error callback (if using nats.ErrorHandler):
nc.SetErrorHandler(func(conn *nats.Conn, sub *nats.Subscription, err error) {
    CaptureError(err, map[string]string{
        "operation": "nats",
        "subject":   sub.Subject,
    })
})
```

- [ ] **Step 6: Run tests**

Run: `cd /path/to/edgesync-notify-app/go && go test ./... -v -count=1`
Expected: PASS (Sentry init with empty DSN is a no-op, tests still pass)

- [ ] **Step 7: Commit**

```bash
git add go/sentry.go go/main.go go/server.go go/bridge.go go/go.mod go/go.sum
git commit -m "feat: add Sentry Go SDK — panic recovery, NATS/sync error capture"
```

---

### Task 11: Sentry React Native — JS + Native Crash Reporting

**Files:**
- Create: `app/lib/sentry.ts`
- Modify: `app/_layout.tsx`
- Modify: `app.json`
- Modify: `package.json`

- [ ] **Step 1: Install @sentry/react-native**

Run:
```bash
cd /path/to/edgesync-notify-app
npx expo install @sentry/react-native
```

- [ ] **Step 2: Add Sentry plugin to app.json**

In `app.json`, add to the `plugins` array:

```json
[
  "@sentry/react-native/expo",
  {
    "organization": "craft-design-group",
    "project": "edgesync-notify-app"
  }
]
```

- [ ] **Step 3: Create sentry.ts init module**

Create `app/lib/sentry.ts`:

```typescript
import * as Sentry from "@sentry/react-native";

const SENTRY_DSN = process.env.EXPO_PUBLIC_SENTRY_DSN ?? "";

export function initSentry() {
  if (!SENTRY_DSN) {
    console.log("Sentry DSN not set, error tracking disabled");
    return;
  }

  Sentry.init({
    dsn: SENTRY_DSN,
    tracesSampleRate: 0.1,
    enableAutoSessionTracking: true,
    attachScreenshot: true,
  });
}

/**
 * Set user/device context after pairing.
 */
export function setSentryContext(opts: {
  project?: string;
  deviceName?: string;
  peerId?: string;
}) {
  if (opts.project) Sentry.setTag("project", opts.project);
  if (opts.deviceName) Sentry.setTag("device", opts.deviceName);
  if (opts.peerId) Sentry.setTag("peer_id", opts.peerId);
}

export { Sentry };
```

- [ ] **Step 4: Wrap app with Sentry in root layout**

In `app/_layout.tsx`:

```typescript
import { initSentry, Sentry } from "./lib/sentry";

// Call initSentry before the component renders (module-level side effect).
initSentry();

// Wrap the default export:
function RootLayout() {
  // ... existing layout code
}

export default Sentry.wrap(RootLayout);
```

- [ ] **Step 5: Add navigation breadcrumbs**

In `app/_layout.tsx`, add navigation integration:

```typescript
import { useNavigationContainerRef } from "expo-router";

const navigationRef = useNavigationContainerRef();

useEffect(() => {
  if (navigationRef) {
    const routingInstrumentation = new Sentry.ReactNavigationInstrumentation();
    routingInstrumentation.registerNavigationContainer(navigationRef);
  }
}, [navigationRef]);
```

- [ ] **Step 6: Set Sentry context after pairing**

In `app/(screens)/index.tsx`, after successful pairing:

```typescript
import { setSentryContext } from "../lib/sentry";

// After pairing, fetch status and set Sentry context:
const status = await (await fetch(`${url}/status`)).json();
setSentryContext({
  project: activeProject,
  deviceName: status.device_name,
  peerId: status.peer_id,
});
```

- [ ] **Step 7: Commit**

```bash
git add app/lib/sentry.ts app/_layout.tsx app/(screens)/index.tsx app.json package.json
git commit -m "feat: add @sentry/react-native — JS exceptions, native crashes, navigation breadcrumbs"
```

---

### Task 12: CI — Add Sentry DSN Secret

**Files:**
- Modify: `.github/workflows/ci.yml`

- [ ] **Step 1: Add SENTRY_DSN to CI environment**

In `.github/workflows/ci.yml`, add to the `env` section:

```yaml
env:
  SENTRY_DSN: ${{ secrets.SENTRY_DSN }}
  EXPO_PUBLIC_SENTRY_DSN: ${{ secrets.SENTRY_DSN }}
```

The Go tests run fine with an empty DSN (Sentry init is a no-op). The React Native TypeScript check (`npx tsc --noEmit`) doesn't need a real DSN either. The secret is only needed for actual error reporting in built apps.

- [ ] **Step 2: Add Sentry project and DSN to Sentry.io**

Manual step:
1. Go to sentry.io, organization: `craft-design-group`
2. Create project: `edgesync-notify-app`, platform: React Native
3. Copy DSN
4. Add as GitHub secret: `SENTRY_DSN`

- [ ] **Step 3: Commit**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: add SENTRY_DSN secret to CI environment"
```

---

## Verification Checklist

After all tasks are complete, verify end-to-end:

- [ ] App launches, Go server starts (check port in Xcode console)
- [ ] Pairing: QR scan or token entry connects to hub
- [ ] Real-time: hub sends message, SSE delivers to Inbox within 1 second
- [ ] Thread detail: messages render as bubbles, actions render as buttons
- [ ] Reply: text reply and action tap both arrive at hub's `watch`
- [ ] Background: kill SSE, send message, reopen — missed messages replayed
- [ ] BGTask: simulate background refresh, verify local notification fires
- [ ] Notification tap: opens correct thread
- [ ] Sentry Go: trigger a panic (e.g., nil pointer in test handler), verify it appears in Sentry
- [ ] Sentry RN: throw a JS error, verify it appears in Sentry with navigation breadcrumbs
- [ ] CI: `go test` and `npx tsc --noEmit` both pass with empty `SENTRY_DSN`
- [ ] Disconnect: Settings > Disconnect calls `POST /stop`, returns to pairing screen
