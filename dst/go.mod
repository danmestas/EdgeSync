module github.com/dmestas/edgesync/dst

go 1.26.0

require (
	github.com/danmestas/go-libfossil v0.2.1
	github.com/dmestas/edgesync/bridge v0.0.0
	github.com/dmestas/edgesync/leaf v0.0.0
)

require (
	github.com/cespare/xxhash/v2 v2.3.0 // indirect
	github.com/felixge/httpsnoop v1.0.4 // indirect
	github.com/go-logr/logr v1.4.3 // indirect
	github.com/go-logr/stdr v1.2.2 // indirect
	github.com/klauspost/compress v1.18.4 // indirect
	github.com/nats-io/nats.go v1.49.0 // indirect
	github.com/nats-io/nkeys v0.4.15 // indirect
	github.com/nats-io/nuid v1.0.1 // indirect
	go.opentelemetry.io/auto/sdk v1.2.1 // indirect
	go.opentelemetry.io/contrib/instrumentation/net/http/otelhttp v0.67.0 // indirect
	go.opentelemetry.io/otel v1.42.0 // indirect
	go.opentelemetry.io/otel/metric v1.42.0 // indirect
	go.opentelemetry.io/otel/trace v1.42.0 // indirect
	golang.org/x/crypto v0.49.0 // indirect
	golang.org/x/sys v0.42.0 // indirect
)

replace (
	github.com/dmestas/edgesync/bridge => ../bridge
	github.com/dmestas/edgesync/leaf => ../leaf
)
