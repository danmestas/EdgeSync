package manifest

import (
	"fmt"
	"time"

	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

type LogOpts struct {
	Start libfossil.FslID
	Limit int
}

type LogEntry struct {
	RID     libfossil.FslID
	UUID    string
	Comment string
	User    string
	Time    time.Time
	Parents []string
}

func Log(r *repo.Repo, opts LogOpts) ([]LogEntry, error) {
	if opts.Start <= 0 {
		return nil, fmt.Errorf("manifest.Log: invalid start rid %d", opts.Start)
	}
	var entries []LogEntry
	current := opts.Start
	for {
		if opts.Limit > 0 && len(entries) >= opts.Limit {
			break
		}
		var uuid, user, comment string
		var mtimeRaw any
		err := r.DB().QueryRow(
			"SELECT b.uuid, e.user, e.comment, e.mtime FROM blob b JOIN event e ON e.objid=b.rid WHERE b.rid=?",
			current,
		).Scan(&uuid, &user, &comment, &mtimeRaw)
		if err != nil {
			return nil, fmt.Errorf("manifest.Log: rid=%d: %w", current, err)
		}
		// mtime is a julianday float. modernc returns float64;
		// ncruces and mattn return time.Time. Handle both.
		var mtime float64
		switch v := mtimeRaw.(type) {
		case float64:
			mtime = v
		case time.Time:
			mtime = libfossil.TimeToJulian(v)
		case int64:
			mtime = float64(v)
		default:
			return nil, fmt.Errorf("manifest.Log: rid=%d: unexpected mtime type %T", current, mtimeRaw)
		}
		var parents []string
		rows, err := r.DB().Query(
			"SELECT b.uuid FROM plink p JOIN blob b ON b.rid=p.pid WHERE p.cid=? ORDER BY p.isprim DESC",
			current,
		)
		if err == nil {
			for rows.Next() {
				var puuid string
				rows.Scan(&puuid)
				parents = append(parents, puuid)
			}
			rows.Close()
		}
		entries = append(entries, LogEntry{
			RID: current, UUID: uuid, Comment: comment,
			User: user, Time: libfossil.JulianToTime(mtime), Parents: parents,
		})
		var parentRid int64
		if err := r.DB().QueryRow(
			"SELECT pid FROM plink WHERE cid=? AND isprim=1", current,
		).Scan(&parentRid); err != nil {
			break
		}
		current = libfossil.FslID(parentRid)
	}
	return entries, nil
}
