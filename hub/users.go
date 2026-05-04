package hub

import (
	"errors"
	"fmt"

	libfossil "github.com/danmestas/libfossil"
)

// User describes a fossil user on the hub repo.
type User struct {
	Login string
	Caps  string
}

// AddUser creates a fossil user. Returns an error if Login is empty or the
// user already exists.
func (h *Hub) AddUser(u User) error {
	if u.Login == "" {
		return errors.New("hub: AddUser: Login is required")
	}
	if err := h.repo.CreateUser(libfossil.UserOpts{Login: u.Login, Caps: u.Caps}); err != nil {
		return fmt.Errorf("hub: add user %q: %w", u.Login, err)
	}
	return nil
}

// GetUser returns the user with the given login. Returns an error if no
// such user exists.
func (h *Hub) GetUser(login string) (User, error) {
	u, err := h.repo.GetUser(login)
	if err != nil {
		return User{}, fmt.Errorf("hub: get user %q: %w", login, err)
	}
	return User{Login: u.Login, Caps: u.Caps}, nil
}

// HasUser reports whether a user with the given login exists.
func (h *Hub) HasUser(login string) bool {
	_, err := h.repo.GetUser(login)
	return err == nil
}

// ListUsers returns all users on the hub.
func (h *Hub) ListUsers() ([]User, error) {
	users, err := h.repo.ListUsers()
	if err != nil {
		return nil, fmt.Errorf("hub: list users: %w", err)
	}
	out := make([]User, len(users))
	for i, u := range users {
		out[i] = User{Login: u.Login, Caps: u.Caps}
	}
	return out, nil
}

// RemoveUser deletes the user with the given login.
func (h *Hub) RemoveUser(login string) error {
	if err := h.repo.DeleteUser(login); err != nil {
		return fmt.Errorf("hub: remove user %q: %w", login, err)
	}
	return nil
}
