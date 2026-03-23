module github.com/dmestas/edgesync/go-libfossil/db/driver/mattn

go 1.26.0

require (
	github.com/dmestas/edgesync/go-libfossil v0.0.0
	github.com/mattn/go-sqlite3 v1.14.34
)

replace (
	github.com/dmestas/edgesync/go-libfossil => ../../../
	github.com/dmestas/edgesync/go-libfossil/db/driver/modernc => ../modernc
	github.com/dmestas/edgesync/go-libfossil/db/driver/ncruces => ../ncruces
)
