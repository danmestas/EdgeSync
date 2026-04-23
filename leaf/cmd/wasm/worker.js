// Web Worker wrapper for EdgeSync WASM leaf agent.
// Usage: new Worker("worker.js")
// Messages: { type: "init", config: {...} }, { type: "start" }, { type: "sync" }, { type: "stop" }

importScripts("wasm_exec.js");

let go;

self.onmessage = async function(e) {
    const msg = e.data;

    switch (msg.type) {
        case "init":
            go = new Go();
            const result = await WebAssembly.instantiateStreaming(
                fetch(msg.wasmUrl || "leaf-browser.wasm"),
                go.importObject
            );
            go.run(result.instance);
            const initResult = edgesync.newAgent(msg.config || {});
            self.postMessage({ type: "init", result: initResult });
            break;

        case "start":
            self.postMessage({ type: "started", result: edgesync.start() });
            break;

        case "sync":
            self.postMessage({ type: "synced", result: edgesync.syncNow() });
            break;

        case "stop":
            self.postMessage({ type: "stopped", result: edgesync.stop() });
            break;
    }
};
