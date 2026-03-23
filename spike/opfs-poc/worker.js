// Web Worker — runs Go WASM with OPFS-backed Fossil repo.
// OPFS createSyncAccessHandle() only works in a dedicated Worker.

const DB_NAME = "repo.fossil";

function log(msg) {
    postMessage({ type: "log", text: msg });
}

async function init() {
    try {
        log("Getting OPFS root directory...");
        const root = await navigator.storage.getDirectory();

        log("Getting directory handle for sqlite3-opfs...");
        const dir = await root.getDirectoryHandle("sqlite3-opfs", { create: true });

        const handles = {};
        for (const suffix of ["", "-journal", "-wal"]) {
            const name = DB_NAME + suffix;
            log("Creating sync access handle for " + name + "...");
            const fh = await dir.getFileHandle(name, { create: true });
            handles[name] = await fh.createSyncAccessHandle();
        }
        log("Got sync handles.");

        log("Loading Go WASM runtime...");
        importScripts("wasm_exec.js");

        const go = new Go();
        const result = await WebAssembly.instantiateStreaming(fetch("poc.wasm"), go.importObject);

        log("Starting Go WASM...");
        go.run(result.instance);

        log("Registering OPFS handles...");
        _opfs_init(handles);

        _poc_ready();

    } catch (e) {
        log("Init failed: " + e.message + "\n" + e.stack);
        postMessage({ type: "error", text: e.message });
    }
}

// --- OPFS Checkout File Manager ---
// Expose checkout functions on self so Go's js.Global().Call() can find them.

let checkoutRoot = null;

async function getCheckoutRoot() {
    if (checkoutRoot) return checkoutRoot;
    const root = await navigator.storage.getDirectory();
    checkoutRoot = await root.getDirectoryHandle("checkout", { create: true });
    return checkoutRoot;
}

async function navigateTo(root, path) {
    const parts = path.split("/").filter(p => p && p !== "." && p !== "..");
    let dir = root;
    for (const part of parts) {
        dir = await dir.getDirectoryHandle(part, { create: true });
    }
    return dir;
}

async function navigateToParent(root, path) {
    const parts = path.split("/").filter(p => p && p !== "." && p !== "..");
    const fileName = parts.pop();
    let dir = root;
    for (const part of parts) {
        dir = await dir.getDirectoryHandle(part, { create: true });
    }
    return [dir, fileName];
}

function _opfs_co_write(id, path, contentArray) {
    (async () => {
        try {
            const root = await getCheckoutRoot();
            const [dir, name] = await navigateToParent(root, path);
            const fh = await dir.getFileHandle(name, { create: true });
            const writable = await fh.createWritable();
            await writable.write(contentArray);
            await writable.close();
            _go_co_resolve(id, null, null);
        } catch (e) {
            _go_co_resolve(id, e.message, null);
        }
    })();
}

function _opfs_co_read(id, path) {
    (async () => {
        try {
            const root = await getCheckoutRoot();
            const [dir, name] = await navigateToParent(root, path);
            const fh = await dir.getFileHandle(name);
            const file = await fh.getFile();
            const buf = await file.arrayBuffer();
            const text = new TextDecoder().decode(new Uint8Array(buf));
            _go_co_resolve(id, null, text);
        } catch (e) {
            _go_co_resolve(id, e.message, null);
        }
    })();
}

function _opfs_co_list(id, dirPath) {
    (async () => {
        try {
            const root = await getCheckoutRoot();
            let dir = root;
            if (dirPath && dirPath !== "" && dirPath !== ".") {
                dir = await navigateTo(root, dirPath);
            }
            const entries = [];
            for await (const [name, handle] of dir.entries()) {
                if (name === ".fossil-checkout") continue;
                if (handle.kind === "file") {
                    const file = await handle.getFile();
                    entries.push({ name, isDir: false, size: file.size });
                } else {
                    entries.push({ name, isDir: true, size: 0 });
                }
            }
            _go_co_resolve(id, null, JSON.stringify(entries));
        } catch (e) {
            _go_co_resolve(id, e.message, null);
        }
    })();
}

function _opfs_co_delete(id, path) {
    (async () => {
        try {
            const root = await getCheckoutRoot();
            const [dir, name] = await navigateToParent(root, path);
            await dir.removeEntry(name);
            _go_co_resolve(id, null, null);
        } catch (e) {
            _go_co_resolve(id, e.message, null);
        }
    })();
}

function _opfs_co_clear(id) {
    (async () => {
        try {
            const opfsRoot = await navigator.storage.getDirectory();
            try {
                await opfsRoot.removeEntry("checkout", { recursive: true });
            } catch (e) { /* might not exist */ }
            checkoutRoot = null;
            _go_co_resolve(id, null, null);
        } catch (e) {
            _go_co_resolve(id, e.message, null);
        }
    })();
}

function _opfs_co_stat(id, path) {
    (async () => {
        try {
            const root = await getCheckoutRoot();
            if (!path || path === "" || path === ".") {
                _go_co_resolve(id, null, JSON.stringify({ exists: true, isDir: true, size: 0 }));
                return;
            }
            const parts = path.split("/").filter(p => p && p !== ".");
            let dir = root;
            try {
                for (const part of parts) {
                    dir = await dir.getDirectoryHandle(part);
                }
                _go_co_resolve(id, null, JSON.stringify({ exists: true, isDir: true, size: 0 }));
                return;
            } catch (e) {}
            try {
                const [parentDir, name] = await navigateToParent(root, path);
                const fh = await parentDir.getFileHandle(name);
                const file = await fh.getFile();
                _go_co_resolve(id, null, JSON.stringify({ exists: true, isDir: false, size: file.size }));
            } catch (e) {
                _go_co_resolve(id, null, JSON.stringify({ exists: false, isDir: false, size: 0 }));
            }
        } catch (e) {
            _go_co_resolve(id, e.message, null);
        }
    })();
}

// Explicitly bind to self for Go's js.Global().Call().
self._opfs_co_write = _opfs_co_write;
self._opfs_co_read = _opfs_co_read;
self._opfs_co_list = _opfs_co_list;
self._opfs_co_delete = _opfs_co_delete;
self._opfs_co_clear = _opfs_co_clear;
self._opfs_co_stat = _opfs_co_stat;

// --- Message Handlers ---

self.onmessage = function(e) {
    const msg = e.data;
    try {
        switch (msg.type) {
            case "clone":     _clone(); break;
            case "sync":      _sync(); break;
            case "status":    _status(); break;
            case "timeline":  _timeline(); break;
            case "files":     _files(msg.rid || "tip"); break;
            case "readFile":  _readFile(msg.uuid); break;
            case "commit":    _commit(msg.filesJSON, msg.comment, msg.user); break;
            case "startAgent": _startAgent(msg.url || ""); break;
            case "stopAgent":  _stopAgent(); break;
            case "syncNow":    _syncNow(); break;
            case "checkout":  _checkout(); break;
            case "coFiles":   _co_files(msg.path || ""); break;
            case "coRead":    _co_read(msg.path); break;
            case "coWrite":   _co_write(msg.path, msg.content); break;
            case "coDelete":  _co_delete(msg.path); break;
            case "coStatus":  _co_status(); break;
            case "coCommit":  _co_commit(msg.comment, msg.user); break;
        }
    } catch (e) {
        postMessage({ type: "error", text: e.message });
    }
};

init();
