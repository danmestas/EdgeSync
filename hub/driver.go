package hub

// libfossil's db layer requires a SQL driver to be registered before any
// Repo can be opened. Registering it here means consumers of EdgeSync can
// `import "github.com/danmestas/EdgeSync/hub"` without also taking a direct
// dependency on libfossil's internal SQL machinery — the package doc
// promise that consumers don't need to import libfossil holds end-to-end.
//
// modernc is the production default. If a consumer ever needs ncruces
// (WASM/WASI), expose an escape hatch then — don't preemptively widen
// the API surface for a hypothetical caller.
import _ "github.com/danmestas/libfossil/db/driver/modernc"
