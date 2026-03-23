# EdgeSync Browser Playground — Architecture Diagrams

## System Architecture

```mermaid
graph TB
    subgraph Browser["Browser Tab (Web Worker)"]
        direction TB
        UI["index.html<br/>3-column IDE layout"]
        Worker["worker.js<br/>Message dispatch"]
        WASM["Go WASM Binary"]

        subgraph GoModules["Go Modules"]
            Main["main.go<br/>Handler dispatch"]
            Bridge["bridge.go<br/>Go↔JS async bridge"]
            Checkout["checkout.go<br/>OPFS file manager"]
            Social["social.go<br/>Chat + Presence"]
        end

        subgraph OPFS["OPFS Storage"]
            DB["sqlite3-opfs/<br/>repo.fossil<br/>(sync access handles)"]
            Files["checkout/<br/>README.md<br/>src/main.go<br/>(async file API)"]
            Meta[".fossil-checkout<br/>(tip RID)"]
        end

        UI <-->|postMessage| Worker
        Worker <--> WASM
        WASM --- GoModules
        Main --> Bridge
        Main --> Checkout
        Main --> Social
        Checkout --> Bridge
        Bridge --> DB
        Checkout -->|coCall| Files
        Checkout --> Meta
    end

    subgraph Server["serve.go (Native)"]
        Serve["HTTP Server<br/>:random port"]
        NATS["Embedded NATS<br/>:4222 TCP<br/>:8222 WebSocket"]
        Proxy["/proxy endpoint"]
        Remote["/remote-info"]
        Static["Static files<br/>+ COOP/COEP headers"]
    end

    subgraph External["External Processes"]
        Fossil["fossil serve :9091<br/>(clone protocol)"]
        Leaf["Native Leaf Agent<br/>:9090 HTTP<br/>NATS client + server"]
        RepoFile[("repo.fossil<br/>SQLite file")]
    end

    WASM -->|"WSDialer ws://:8222"| NATS
    WASM -->|"HTTP /proxy"| Serve
    Proxy --> Fossil
    Leaf <-->|"nats://:4222"| NATS
    Fossil --- RepoFile
    Leaf --- RepoFile
    Remote -->|"crosslink + query"| RepoFile

    style Browser fill:#1a1e2e,stroke:#2a3040,color:#e8ecf4
    style OPFS fill:#0c0e14,stroke:#5b9bd5,color:#a0aabe
    style GoModules fill:#12151e,stroke:#2a3040,color:#e8ecf4
    style Server fill:#12151e,stroke:#e5a844,color:#e8ecf4
    style External fill:#12151e,stroke:#5cb870,color:#e8ecf4
```

## Go↔JS Async Bridge

```mermaid
sequenceDiagram
    participant Go as Go Goroutine
    participant Map as sync.Map<br/>(coPending)
    participant JS as JS Event Loop
    participant OPFS as OPFS API

    Go->>Map: Store(id, chan)
    Go->>JS: js.Global().Call("_opfs_co_write", id, path, data)
    Note over Go: Blocks on <-ch<br/>(30s timeout)

    JS->>OPFS: createWritable() + write()
    OPFS-->>JS: Promise resolves

    JS->>Map: _go_co_resolve(id, null, result)
    Map-->>Go: ch <- coResult{data}
    Note over Go: Unblocks, returns data

    alt Timeout (30s)
        Go->>Map: Delete(id)
        Note over Go: Returns error
    end
```

## Clone → Checkout → Sync Flow

```mermaid
stateDiagram-v2
    [*] --> Empty: Page Load
    Empty --> Cloning: Clone button

    state Cloning {
        [*] --> FetchBlobs: HTTP POST /proxy
        FetchBlobs --> Crosslink: blobs received
        Crosslink --> Materialize: events populated
        Materialize --> WriteFiles: tip → OPFS files
        WriteFiles --> [*]
    }

    Cloning --> Ready: Auto-checkout complete

    state Ready {
        [*] --> Idle

        Idle --> Editing: Click file
        Editing --> Saving: Save button
        Saving --> Idle: coCall _opfs_co_write
        Editing --> Creating: + New File
        Creating --> Idle: coCall _opfs_co_write
        Editing --> Deleting: - Delete
        Deleting --> Idle: coCall _opfs_co_delete

        Idle --> Committing: Commit button
        Committing --> Idle: manifest.Checkin

        Idle --> AgentStart: Start Agent
        AgentStart --> Syncing: WSDialer → NATS
    }

    state Syncing {
        [*] --> Connected
        Connected --> SyncRound: Every 10s
        SyncRound --> PushCheck: igot cards
        PushCheck --> FileSend: gimme response
        FileSend --> PullCheck: file cards sent
        PullCheck --> CrosslinkPull: files received
        CrosslinkPull --> ReCheckout: new tip detected
        ReCheckout --> Connected: OPFS refreshed
        Connected --> [*]: Stop Agent
    }
```

## NATS Message Routing

```mermaid
graph LR
    subgraph BrowserA["Browser Tab A"]
        AgentA["Leaf Agent<br/>(push/pull)"]
        ChatA["Chat Publisher"]
        PresA["Presence<br/>Heartbeat"]
    end

    subgraph NATS["NATS Server<br/>(embedded in serve.go)"]
        Sync["fossil.&lt;proj&gt;.sync<br/>(request/reply)"]
        ChatSub["edgesync.chat<br/>(pub/sub)"]
        PresSub["edgesync.presence<br/>(pub/sub)"]
    end

    subgraph BrowserB["Browser Tab B"]
        AgentB["Leaf Agent"]
        ChatB["Chat Subscriber"]
        PresB["Presence<br/>Listener"]
    end

    subgraph NativeLeaf["Native Leaf"]
        NAgent["Agent + ServeNATS"]
    end

    AgentA <-->|xfer cards| Sync
    AgentB <-->|xfer cards| Sync
    NAgent <-->|xfer cards| Sync

    ChatA -->|publish| ChatSub
    ChatSub -->|deliver| ChatB
    ChatSub -->|deliver| ChatA

    PresA -->|heartbeat 5s| PresSub
    PresB -->|heartbeat 5s| PresSub
    PresSub -->|deliver| PresA
    PresSub -->|deliver| PresB

    style NATS fill:#e5a844,stroke:#9e7530,color:#0c0e14
```

## OPFS Storage Layout

```mermaid
graph TD
    subgraph Root["OPFS Root (navigator.storage)"]
        subgraph SQLite["sqlite3-opfs/"]
            DB["repo.fossil<br/>368KB<br/>(sync access handle)"]
            Journal["repo.fossil-journal<br/>(sync access handle)"]
            WAL["repo.fossil-wal<br/>(sync access handle)"]
        end

        subgraph Checkout["checkout/"]
            Meta[".fossil-checkout<br/>tip RID metadata"]
            F1["README.md"]
            F2["second.txt"]
            subgraph SubDir["src/"]
                F3["main.go"]
            end
            subgraph SubDir2["docs/"]
                F4["guide.md"]
            end
        end
    end

    SQLite -.->|"createSyncAccessHandle()<br/>Worker-only, synchronous"| Note1["Pre-opened at init<br/>One handle per file"]
    Checkout -.->|"getDirectoryHandle()<br/>createWritable()<br/>getFile()"| Note2["Dynamic creation<br/>Async API via coCall bridge"]

    style SQLite fill:#5b9bd5,stroke:#3a6d9e,color:#0c0e14
    style Checkout fill:#5cb870,stroke:#3a8a50,color:#0c0e14
    style Root fill:#1a1e2e,stroke:#2a3040,color:#e8ecf4
```

## File Structure

```mermaid
graph LR
    subgraph spike/opfs-poc
        main["main.go<br/>510 lines<br/>WASM entry + handlers"]
        bridge["bridge.go<br/>88 lines<br/>Go↔JS async bridge"]
        checkout["checkout.go<br/>290 lines<br/>OPFS checkout manager"]
        social["social.go<br/>215 lines<br/>Chat + Presence"]
        serve["serve.go<br/>240 lines<br/>Native dev server"]
        worker["worker.js<br/>230 lines<br/>Web Worker + OPFS ops"]
        html["index.html<br/>750 lines<br/>3-column IDE UI"]
        gomod["go.mod<br/>Dependencies"]
    end

    main --> bridge
    main --> checkout
    main --> social
    checkout --> bridge
    serve -.->|"builds"| main
    worker -.->|"loads"| main

    style main fill:#e5a844,stroke:#9e7530,color:#0c0e14
    style bridge fill:#5b9bd5,stroke:#3a6d9e,color:#0c0e14
    style checkout fill:#5cb870,stroke:#3a8a50,color:#0c0e14
    style social fill:#e05252,stroke:#a03030,color:#0c0e14
    style serve fill:#e5a844,stroke:#9e7530,color:#0c0e14
```
