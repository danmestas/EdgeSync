package main

import (
	"crypto/rand"
	"encoding/hex"
	"fmt"

	"github.com/danmestas/go-libfossil/auth"
	"github.com/danmestas/go-libfossil/repo"
)

type RepoUserCmd struct {
	Add    UserAddCmd    `cmd:"" help:"Create a new user"`
	List   UserListCmd   `cmd:"" help:"List all users"`
	Update UserUpdateCmd `cmd:"" help:"Update user capabilities"`
	Rm     UserRmCmd     `cmd:"" help:"Delete a user"`
	Passwd UserPasswdCmd `cmd:"" help:"Reset user password"`
}

type UserAddCmd struct {
	Login string `arg:"" help:"Username"`
	Cap   string `help:"Capability string (e.g. oi)" required:""`
}

func (c *UserAddCmd) Run(g *Globals) error {
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
	fmt.Printf("Created user %q (caps: %s)\n", c.Login, c.Cap)
	fmt.Printf("Password: %s\n", password)
	return nil
}

type UserListCmd struct{}

func (c *UserListCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	users, err := auth.ListUsers(r.DB())
	if err != nil {
		return err
	}
	fmt.Printf("%-20s %-20s\n", "LOGIN", "CAPABILITIES")
	for _, u := range users {
		fmt.Printf("%-20s %-20s\n", u.Login, u.Cap)
	}
	return nil
}

type UserUpdateCmd struct {
	Login string `arg:"" help:"Username"`
	Cap   string `help:"New capability string" required:""`
}

func (c *UserUpdateCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := auth.UpdateCaps(r.DB(), c.Login, c.Cap); err != nil {
		return err
	}
	fmt.Printf("Updated %q capabilities: %s\n", c.Login, c.Cap)
	return nil
}

type UserRmCmd struct {
	Login string `arg:"" help:"Username"`
}

func (c *UserRmCmd) Run(g *Globals) error {
	r, err := openRepo(g)
	if err != nil {
		return err
	}
	defer r.Close()

	if err := auth.DeleteUser(r.DB(), c.Login); err != nil {
		return err
	}
	fmt.Printf("Deleted user %q\n", c.Login)
	return nil
}

type UserPasswdCmd struct {
	Login string `arg:"" help:"Username"`
}

func (c *UserPasswdCmd) Run(g *Globals) error {
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

	if err := auth.SetPassword(r.DB(), pc, c.Login, password); err != nil {
		return err
	}
	fmt.Printf("New password for %q: %s\n", c.Login, password)
	return nil
}

func generatePassword() (string, error) {
	b := make([]byte, 32)
	if _, err := rand.Read(b); err != nil {
		return "", fmt.Errorf("generating password: %w", err)
	}
	return hex.EncodeToString(b), nil
}

func repoProjectCode(r *repo.Repo) (string, error) {
	var pc string
	err := r.DB().QueryRow("SELECT value FROM config WHERE name='project-code'").Scan(&pc)
	if err != nil {
		return "", fmt.Errorf("reading project-code: %w", err)
	}
	return pc, nil
}
