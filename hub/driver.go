//go:build !js && !wasm

package hub

// libfossil's db layer requires a SQL driver to be registered before any
// Repo can be opened. Registering it here means consumers of EdgeSync can
// `import "github.com/danmestas/EdgeSync/hub"` without also taking a direct
// dependency on libfossil's internal SQL machinery — the package doc
// promise that consumers don't need to import libfossil holds end-to-end.
//
// modernc is a pure-Go cgo-free SQLite driver suitable for every supported
// GOOS/GOARCH except js/wasm — its generated code references unsupported
// syscalls under those targets. The build constraint above excludes this
// file from wasm compilation; wasm consumers must register their own
// driver (libfossil's ncruces driver has wasm support via ncruces_js.go).
import _ "github.com/danmestas/libfossil/db/driver/modernc"
