module github.com/dmestas/edgesync/dst

go 1.26.0

require (
	github.com/dmestas/edgesync/bridge v0.0.0
	github.com/dmestas/edgesync/go-libfossil v0.0.0
	github.com/dmestas/edgesync/leaf v0.0.0
)

require (
	github.com/klauspost/compress v1.18.4 // indirect
	github.com/nats-io/nats.go v1.49.0 // indirect
	github.com/nats-io/nkeys v0.4.15 // indirect
	github.com/nats-io/nuid v1.0.1 // indirect
	golang.org/x/crypto v0.49.0 // indirect
	golang.org/x/sys v0.42.0 // indirect
)

replace (
	github.com/dmestas/edgesync/bridge => ../bridge
	github.com/dmestas/edgesync/go-libfossil => ../go-libfossil
	github.com/dmestas/edgesync/go-libfossil/db/driver/mattn => ../go-libfossil/db/driver/mattn
	github.com/dmestas/edgesync/go-libfossil/db/driver/modernc => ../go-libfossil/db/driver/modernc
	github.com/dmestas/edgesync/go-libfossil/db/driver/ncruces => ../go-libfossil/db/driver/ncruces
	github.com/dmestas/edgesync/leaf => ../leaf
)
