package manifest

import (
	libfossil "github.com/dmestas/edgesync/go-libfossil"
	"github.com/dmestas/edgesync/go-libfossil/content"
	"github.com/dmestas/edgesync/go-libfossil/deck"
	"github.com/dmestas/edgesync/go-libfossil/repo"
)

// AfterDephantomize crosslinks a formerly-phantom blob and any dependents.
// Matches Fossil's after_dephantomize (content.c:389-456).
func AfterDephantomize(r *repo.Repo, rid libfossil.FslID) {
	if r == nil {
		panic("manifest.AfterDephantomize: r must not be nil")
	}
	if rid <= 0 {
		return
	}
	afterDephantomize(r, rid, true)
}

func afterDephantomize(r *repo.Repo, rid libfossil.FslID, linkFlag bool) {
	for rid > 0 {
		if linkFlag {
			crosslinkSingle(r, rid)
		}

		// Process orphaned delta manifests whose baseline is this rid.
		orphanRows, err := r.DB().Query("SELECT rid FROM orphan WHERE baseline=?", rid)
		if err == nil {
			var orphans []libfossil.FslID
			for orphanRows.Next() {
				var orid int64
				if orphanRows.Scan(&orid) == nil {
					orphans = append(orphans, libfossil.FslID(orid))
				}
			}
			orphanRows.Close()
			for _, orid := range orphans {
				crosslinkSingle(r, orid)
			}
			if len(orphans) > 0 {
				r.DB().Exec("DELETE FROM orphan WHERE baseline=?", rid)
			}
		}

		// Find delta children not yet crosslinked.
		childRows, err := r.DB().Query(
			`SELECT rid FROM delta WHERE srcid=? AND NOT EXISTS (SELECT 1 FROM mlink WHERE mid=delta.rid)`, rid)
		if err != nil {
			return
		}
		var children []libfossil.FslID
		for childRows.Next() {
			var crid int64
			if childRows.Scan(&crid) == nil {
				children = append(children, libfossil.FslID(crid))
			}
		}
		childRows.Close()

		for i := 1; i < len(children); i++ {
			afterDephantomize(r, children[i], true)
		}
		if len(children) > 0 {
			rid = children[0]
			linkFlag = true
		} else {
			rid = 0
		}
	}
}

// crosslinkSingle crosslinks a single blob by rid.
func crosslinkSingle(r *repo.Repo, rid libfossil.FslID) {
	data, err := content.Expand(r.DB(), rid)
	if err != nil {
		return
	}
	d, err := deck.Parse(data)
	if err != nil {
		return
	}
	switch d.Type {
	case deck.Checkin:
		crosslinkCheckin(r, rid, d)
	case deck.Wiki:
		crosslinkWiki(r, rid, d)
	case deck.Ticket:
		crosslinkTicket(r, rid, d)
	case deck.Event:
		crosslinkEvent(r, rid, d)
	case deck.Attachment:
		crosslinkAttachment(r, rid, d)
	case deck.Cluster:
		crosslinkCluster(r, rid, d)
	case deck.ForumPost:
		crosslinkForum(r, rid, d)
	case deck.Control:
		crosslinkControl(r, rid, d)
	}
}
