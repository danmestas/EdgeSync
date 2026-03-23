//go:build js

package main

import (
	"context"
	"encoding/json"
	"fmt"
	"syscall/js"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	_ "github.com/dmestas/edgesync/go-libfossil/db/driver/ncruces"
	"github.com/dmestas/edgesync/go-libfossil/manifest"
	"github.com/dmestas/edgesync/go-libfossil/repo"
	"github.com/dmestas/edgesync/go-libfossil/simio"
	"github.com/dmestas/edgesync/go-libfossil/sync"
	"github.com/dmestas/edgesync/leaf/agent"
	"github.com/dmestas/edgesync/leaf/wsdialer"
	"github.com/nats-io/nats.go"
)

const repoPath = "repo.fossil"

var (
	currentRepo     *repo.Repo
	currentCheckout *Checkout
	currentAgent    *agent.Agent
	currentNATS     *nats.Conn
)

func log(msg string) {
	js.Global().Get("console").Call("log", "[go]", msg)
	self := js.Global().Get("postMessage")
	if !self.IsUndefined() {
		obj := js.Global().Get("Object").New()
		obj.Set("type", "log")
		obj.Set("text", "[go] "+msg)
		js.Global().Call("postMessage", obj)
	}
}

func postResult(kind, data string) {
	obj := js.Global().Get("Object").New()
	obj.Set("type", "result")
	obj.Set("kind", kind)
	obj.Set("text", data)
	js.Global().Call("postMessage", obj)
}

func postError(text string) {
	obj := js.Global().Get("Object").New()
	obj.Set("type", "error")
	obj.Set("text", text)
	js.Global().Call("postMessage", obj)
}

func toJSON(v any) string {
	out, err := json.Marshal(v)
	if err != nil {
		panic(fmt.Sprintf("toJSON: marshal failed: %v", err))
	}
	return string(out)
}

func ensureRepo() bool {
	if currentRepo != nil {
		return true
	}
	r, err := repo.Open(repoPath)
	if err != nil {
		return false
	}
	currentRepo = r
	log("reopened existing repo from OPFS")
	return true
}

func tipRID() (libfossil.FslID, error) {
	var rid int64
	err := currentRepo.DB().QueryRow(`
		SELECT l.rid FROM leaf l
		JOIN event e ON e.objid=l.rid
		WHERE e.type='ci'
		ORDER BY e.mtime DESC LIMIT 1
	`).Scan(&rid)
	if err != nil {
		return 0, fmt.Errorf("no checkins found: %w", err)
	}
	return libfossil.FslID(rid), nil
}

// dropAllTables clears the OPFS repo so re-clone works.
func dropAllTables() {
	r, err := repo.Open(repoPath)
	if err != nil {
		return
	}
	defer r.Close()
	rows, err := r.DB().Query("SELECT name FROM sqlite_master WHERE type='table'")
	if err != nil {
		return
	}
	var tables []string
	for rows.Next() {
		var name string
		_ = rows.Scan(&name)
		tables = append(tables, name)
	}
	rows.Close()
	for _, t := range tables {
		_, _ = r.DB().Exec("DROP TABLE IF EXISTS [" + t + "]")
	}
}

// autoCheckout materializes the tip checkin into the OPFS working tree.
func autoCheckout(r *repo.Repo) {
	tip, tipErr := tipRID()
	if tipErr != nil {
		log(fmt.Sprintf("auto-checkout skipped: %v", tipErr))
		return
	}
	log(fmt.Sprintf("auto-checkout: materializing tip rid=%d...", tip))
	co := NewCheckout(r, tip)
	n, coErr := co.Materialize()
	if coErr != nil {
		log(fmt.Sprintf("auto-checkout failed: %v", coErr))
		return
	}
	currentCheckout = co
	log(fmt.Sprintf("checked out %d files to OPFS", n))
}

func doClone() {
	if currentRepo != nil {
		currentRepo.Close()
		currentRepo = nil
	}
	currentCheckout = nil
	dropAllTables()

	log("cloning from remote via proxy...")
	r, result, err := sync.Clone(context.Background(), repoPath,
		&sync.HTTPTransport{URL: "/proxy"}, sync.CloneOpts{Env: simio.RealEnv()})
	if err != nil {
		postError(fmt.Sprintf("clone failed: %v", err))
		return
	}
	currentRepo = r

	log(fmt.Sprintf("crosslinked %d checkin(s)", result.CheckinsLinked))
	autoCheckout(r)

	log(fmt.Sprintf("clone complete: %d rounds, %d blobs", result.Rounds, result.BlobsRecvd))
	postResult("clone", toJSON(map[string]any{
		"rounds":      result.Rounds,
		"blobs":       result.BlobsRecvd,
		"projectCode": result.ProjectCode,
	}))
}

func doSync() {
	if !ensureRepo() {
		postError("no repo — clone first")
		return
	}

	log("syncing with remote via proxy...")
	transport := &sync.HTTPTransport{URL: "/proxy"}

	var projectCode, serverCode string
	_ = currentRepo.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projectCode)
	_ = currentRepo.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&serverCode)

	result, err := sync.Sync(context.Background(), currentRepo, transport, sync.SyncOpts{
		Pull:        true,
		Push:        true,
		ProjectCode: projectCode,
		ServerCode:  serverCode,
		Env:         simio.RealEnv(),
	})
	if err != nil {
		postError(fmt.Sprintf("sync failed: %v", err))
		return
	}

	log(fmt.Sprintf("sync complete: %d rounds, sent=%d recv=%d", result.Rounds, result.FilesSent, result.FilesRecvd))
	postResult("sync", toJSON(map[string]any{
		"rounds":   result.Rounds,
		"sent":     result.FilesSent,
		"received": result.FilesRecvd,
	}))
}

func doStatus() {
	if !ensureRepo() {
		postResult("status", toJSON(map[string]any{"open": false}))
		return
	}

	// Restore checkout from OPFS if it exists.
	if currentCheckout == nil && HasCheckout() {
		storedTip, err := ReadCheckoutTipRID()
		if err == nil {
			currentCheckout = NewCheckout(currentRepo, storedTip)
			log("restored checkout from OPFS")
		}
	}

	var blobCount, checkinCount int
	_ = currentRepo.DB().QueryRow("SELECT count(*) FROM blob").Scan(&blobCount)
	_ = currentRepo.DB().QueryRow("SELECT count(*) FROM event WHERE type='ci'").Scan(&checkinCount)

	var projectCode string
	_ = currentRepo.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projectCode)

	postResult("status", toJSON(map[string]any{
		"open":        true,
		"blobs":       blobCount,
		"checkins":    checkinCount,
		"projectCode": projectCode,
		"hasCheckout": currentCheckout != nil,
	}))
}

func doTimeline() {
	if !ensureRepo() {
		postError("no repo — clone first")
		return
	}

	tip, err := tipRID()
	if err != nil {
		postError(err.Error())
		return
	}

	entries, err := manifest.Log(currentRepo, manifest.LogOpts{Start: tip, Limit: 50})
	if err != nil {
		postError(fmt.Sprintf("timeline failed: %v", err))
		return
	}

	type entry struct {
		RID     int64    `json:"rid"`
		UUID    string   `json:"uuid"`
		Comment string   `json:"comment"`
		User    string   `json:"user"`
		Time    string   `json:"time"`
		Parents []string `json:"parents"`
	}
	var result []entry
	for _, e := range entries {
		result = append(result, entry{
			RID:     int64(e.RID),
			UUID:    e.UUID,
			Comment: e.Comment,
			User:    e.User,
			Time:    e.Time.UTC().Format(time.RFC3339),
			Parents: e.Parents,
		})
	}

	postResult("timeline", toJSON(result))
}

// --- OPFS Checkout Handlers ---

func doCheckout() {
	if !ensureRepo() {
		postError("no repo — clone first")
		return
	}
	tip, err := tipRID()
	if err != nil {
		postError(err.Error())
		return
	}

	co := NewCheckout(currentRepo, tip)
	n, err := co.Materialize()
	if err != nil {
		postError(fmt.Sprintf("checkout failed: %v", err))
		return
	}
	currentCheckout = co
	log(fmt.Sprintf("checked out %d files to OPFS", n))
	postResult("checkout", toJSON(map[string]any{"files": n}))
}

func doCoFiles(path string) {
	if currentCheckout == nil {
		postError("no checkout — click Checkout first")
		return
	}
	entries, err := currentCheckout.ListDir(path)
	if err != nil {
		postError(fmt.Sprintf("list dir failed: %v", err))
		return
	}
	postResult("coFiles", toJSON(map[string]any{"path": path, "entries": entries}))
}

func doCoRead(path string) {
	if currentCheckout == nil {
		postError("no checkout")
		return
	}
	data, err := currentCheckout.ReadFile(path)
	if err != nil {
		postError(fmt.Sprintf("read failed: %v", err))
		return
	}
	postResult("coRead", toJSON(map[string]any{"path": path, "content": data}))
}

func doCoWrite(path, fileContent string) {
	if currentCheckout == nil {
		postError("no checkout")
		return
	}
	if err := currentCheckout.WriteFile(path, fileContent); err != nil {
		postError(fmt.Sprintf("write failed: %v", err))
		return
	}
	log(fmt.Sprintf("saved %s to OPFS (%d bytes)", path, len(fileContent)))
	postResult("coWrite", toJSON(map[string]any{"path": path, "size": len(fileContent)}))
}

func doCoDelete(path string) {
	if currentCheckout == nil {
		postError("no checkout")
		return
	}
	if err := currentCheckout.DeleteFile(path); err != nil {
		postError(fmt.Sprintf("delete failed: %v", err))
		return
	}
	log(fmt.Sprintf("deleted %s from OPFS", path))
	postResult("coDelete", toJSON(map[string]any{"path": path}))
}

func doCoStatus() {
	if currentCheckout == nil {
		postError("no checkout")
		return
	}
	changes, err := currentCheckout.Status()
	if err != nil {
		postError(fmt.Sprintf("status failed: %v", err))
		return
	}
	postResult("coStatus", toJSON(changes))
}

func doCoCommit(comment, user string) {
	defer func() {
		if r := recover(); r != nil {
			postError(fmt.Sprintf("commit panic: %v", r))
		}
	}()
	if currentCheckout == nil {
		postError("no checkout")
		return
	}
	rid, uuid, err := currentCheckout.CommitAll(comment, user)
	if err != nil {
		postError(fmt.Sprintf("commit failed: %v", err))
		return
	}
	short := uuid
	if len(short) > 12 {
		short = short[:12]
	}
	log(fmt.Sprintf("committed from OPFS: rid=%d uuid=%s", rid, short))
	postResult("coCommit", toJSON(map[string]any{"rid": int64(rid), "uuid": uuid}))

	// Notify peers so they pull immediately instead of waiting for poll.
	publishNotify(currentNATS, uuid)
}

// --- Agent Handlers ---

// postSyncHook crosslinks pulled manifests and re-materializes checkout.
func postSyncHook(result *sync.SyncResult) {
	if result.FilesRecvd == 0 {
		return
	}
	linked, err := manifest.Crosslink(currentRepo)
	if err != nil {
		log(fmt.Sprintf("[agent] crosslink error: %v", err))
		return
	}
	if linked == 0 {
		return
	}
	log(fmt.Sprintf("[agent] crosslinked %d new checkin(s)", linked))
	if currentCheckout == nil {
		return
	}
	tip, tipErr := tipRID()
	if tipErr != nil || tip == currentCheckout.tipRID {
		return
	}
	log(fmt.Sprintf("[agent] new tip rid=%d, re-materializing checkout...", tip))
	currentCheckout.tipRID = tip
	n, coErr := currentCheckout.Materialize()
	if coErr != nil {
		log(fmt.Sprintf("[agent] re-checkout failed: %v", coErr))
		return
	}
	log(fmt.Sprintf("[agent] checked out %d files from new tip", n))
	postResult("checkout", toJSON(map[string]any{"files": n}))
}

// connectNATS establishes a WebSocket NATS connection.
func connectNATS(wsURL string) (*nats.Conn, error) {
	return nats.Connect("nats://browser",
		nats.Name("edgesync-browser-leaf"),
		nats.SetCustomDialer(&wsdialer.WSDialer{URL: wsURL}),
	)
}

func doStartAgent(natsWsURL string) {
	if currentAgent != nil {
		postError("agent already running")
		return
	}
	if !ensureRepo() {
		postError("no repo — clone first")
		return
	}

	log(fmt.Sprintf("[agent] connecting to %s...", natsWsURL))

	var projectCode, serverCode string
	_ = currentRepo.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&projectCode)
	_ = currentRepo.DB().QueryRow("SELECT value FROM config WHERE name='server-code'").Scan(&serverCode)
	if projectCode == "" {
		postError("agent init failed: no project-code in repo")
		return
	}

	nc, err := connectNATS(natsWsURL)
	if err != nil {
		postError(fmt.Sprintf("agent NATS connect failed: %v", err))
		return
	}
	log("[agent] connected to NATS")

	transport := agent.NewNATSTransport(nc, projectCode, 0, "fossil")
	cfg := agent.Config{
		RepoPath:     repoPath,
		NATSUrl:      "nats://browser",
		PollInterval: 24 * time.Hour, // Notify-driven — poll is just a fallback.
		Push:         true,
		Pull:         true,
		UV:           false, // UV drafts use NATS pub/sub, not UV sync protocol.
		Logger: func(msg string) {
			log("[agent] " + msg)
			postResult("agentLog", toJSON(map[string]any{"msg": msg}))
		},
		PostSyncHook: postSyncHook,
	}
	a := agent.NewFromParts(cfg, currentRepo, transport, projectCode, serverCode)

	if err := a.Start(); err != nil {
		postError(fmt.Sprintf("agent start failed: %v", err))
		nc.Close()
		return
	}

	currentAgent = a
	currentNATS = nc

	user := "browser-" + myPeerID
	if err := startChat(nc, user); err != nil {
		log(fmt.Sprintf("[social] chat start failed: %v", err))
	}
	if err := startPresence(nc, user); err != nil {
		log(fmt.Sprintf("[social] presence start failed: %v", err))
	}
	if err := startNotify(nc); err != nil {
		log(fmt.Sprintf("[social] notify start failed: %v", err))
	}
	if err := startDraftSync(nc); err != nil {
		log(fmt.Sprintf("[social] draft sync start failed: %v", err))
	}

	log("[agent] running — auto-sync every 10s, instant on peer commits")
	postResult("agentState", toJSON(map[string]any{"state": "running", "peerId": myPeerID}))
}

func doStopAgent() {
	if currentAgent == nil {
		postError("agent not running")
		return
	}
	stopSocial()
	stopDraftSync()
	// Close NATS first — this causes in-flight syncs to fail, unblocking
	// the poll loop. We don't call agent.Stop() because it closes the
	// shared repo. Closing NATS is sufficient to halt all sync activity.
	if currentNATS != nil {
		currentNATS.Close()
		currentNATS = nil
	}
	currentAgent = nil
	log("[agent] stopped")
	postResult("agentState", toJSON(map[string]any{"state": "stopped"}))
}

func doSyncNow() {
	if currentAgent == nil {
		postError("agent not running — start agent first")
		return
	}
	currentAgent.SyncNow()
}

// jsFunc wraps a Go handler as a js.Func that runs in a goroutine.
func jsFunc(fn func()) js.Func {
	return js.FuncOf(func(_ js.Value, _ []js.Value) any {
		go fn()
		return nil
	})
}

// jsFuncArgs wraps a Go handler that takes js.Value args.
func jsFuncArgs(fn func([]js.Value)) js.Func {
	return js.FuncOf(func(_ js.Value, args []js.Value) any {
		go fn(args)
		return nil
	})
}

func registerRepoCallbacks() {
	js.Global().Set("_clone", jsFunc(doClone))
	js.Global().Set("_sync", jsFunc(doSync))
	js.Global().Set("_status", jsFunc(doStatus))
	js.Global().Set("_timeline", jsFunc(doTimeline))
}

func registerCheckoutCallbacks() {
	js.Global().Set("_checkout", jsFunc(doCheckout))
	js.Global().Set("_co_files", jsFuncArgs(func(args []js.Value) {
		path := ""
		if len(args) > 0 {
			path = args[0].String()
		}
		doCoFiles(path)
	}))
	js.Global().Set("_co_read", jsFuncArgs(func(args []js.Value) {
		if len(args) < 1 {
			postError("_co_read requires path")
			return
		}
		doCoRead(args[0].String())
	}))
	js.Global().Set("_co_write", jsFuncArgs(func(args []js.Value) {
		if len(args) < 2 {
			postError("_co_write requires path, content")
			return
		}
		doCoWrite(args[0].String(), args[1].String())
	}))
	js.Global().Set("_co_delete", jsFuncArgs(func(args []js.Value) {
		if len(args) < 1 {
			postError("_co_delete requires path")
			return
		}
		doCoDelete(args[0].String())
	}))
	js.Global().Set("_co_status", jsFunc(doCoStatus))
	js.Global().Set("_co_commit", jsFuncArgs(func(args []js.Value) {
		comment, user := "", ""
		if len(args) > 0 {
			comment = args[0].String()
		}
		if len(args) > 1 {
			user = args[1].String()
		}
		doCoCommit(comment, user)
	}))
}

func registerAgentCallbacks() {
	js.Global().Set("_startAgent", jsFuncArgs(func(args []js.Value) {
		url := "ws://localhost:8222"
		if len(args) > 0 && args[0].String() != "" {
			url = args[0].String()
		}
		doStartAgent(url)
	}))
	js.Global().Set("_stopAgent", jsFunc(doStopAgent))
	js.Global().Set("_syncNow", jsFunc(doSyncNow))
}

func main() {
	ready := make(chan struct{})
	js.Global().Set("_poc_ready", js.FuncOf(func(_ js.Value, _ []js.Value) any {
		close(ready)
		return nil
	}))
	<-ready

	registerRepoCallbacks()
	registerCheckoutCallbacks()
	registerAgentCallbacks()
	registerSocialCallbacks()

	log("Fossil OPFS playground ready.")
	js.Global().Call("postMessage", map[string]any{"type": "ready"})
	select {}
}
