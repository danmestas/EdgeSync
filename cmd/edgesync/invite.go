package main

import (
	"fmt"
	"time"

	"github.com/danmestas/go-libfossil/auth"
)

type RepoInviteCmd struct {
	Login string        `arg:"" help:"Username for the invitee"`
	Cap   string        `help:"Capability string (e.g. oi)" required:""`
	URL   string        `help:"Sync URL to embed in token" default:""`
	TTL   time.Duration `help:"Token time-to-live (e.g. 24h)" default:"0"`
}

func (c *RepoInviteCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	password, err := generatePassword()
	if err != nil {
		return err
	}

	pc, err := repoProjectCode(r)
	if err != nil {
		return err
	}

	if err := auth.CreateUser(r.DB(), pc, c.Login, password, c.Cap); err != nil {
		return err
	}

	if c.TTL > 0 {
		expiry := time.Now().Add(c.TTL).Format("2006-01-02 15:04:05")
		if _, err := r.DB().Exec("UPDATE user SET cexpire=? WHERE login=?", expiry, c.Login); err != nil {
			return fmt.Errorf("setting expiry: %w", err)
		}
	}

	url := c.URL
	if url == "" {
		r.DB().QueryRow("SELECT value FROM config WHERE name='last-sync-url'").Scan(&url)
	}

	tok := auth.InviteToken{
		URL:      url,
		Login:    c.Login,
		Password: password,
		Caps:     c.Cap,
	}

	encoded := tok.Encode()

	fmt.Printf("Invite for %q (capabilities: %s", c.Login, c.Cap)
	if c.TTL > 0 {
		fmt.Printf(", expires: %s", time.Now().Add(c.TTL).Format(time.RFC3339))
	}
	fmt.Println("):")
	fmt.Println()
	fmt.Printf("  edgesync repo clone --invite %s\n", encoded)
	fmt.Println()
	fmt.Println("Share this command with the recipient. It contains credentials - treat it like a password.")
	return nil
}
