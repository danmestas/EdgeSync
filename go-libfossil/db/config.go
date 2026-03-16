package db

import "os"

// OpenConfig allows callers to customize driver selection and pragmas.
type OpenConfig struct {
	Driver  string            // override driver name (empty = build-tag default or env var)
	Pragmas map[string]string // additional/override pragmas (merged with defaults)
}

func defaultPragmas() map[string]string {
	return map[string]string{
		"journal_mode": "WAL",
		"busy_timeout": "5000",
	}
}

func driverFromEnv() string {
	if d := os.Getenv("EDGESYNC_SQLITE_DRIVER"); d != "" {
		return d
	}
	return driverName()
}
