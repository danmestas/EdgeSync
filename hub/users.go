package hub

// User describes a fossil user on the hub repo.
type User struct {
	Login string
	Caps  string
}

// AddUser creates a fossil user. Returns an error if Login is empty or the
// user already exists.
func (h *Hub) AddUser(u User) error { return h.repo.AddUser(u) }

// GetUser returns the user with the given login. Returns an error if no
// such user exists.
func (h *Hub) GetUser(login string) (User, error) { return h.repo.GetUser(login) }

// HasUser reports whether a user with the given login exists.
func (h *Hub) HasUser(login string) bool { return h.repo.HasUser(login) }

// ListUsers returns all users on the hub.
func (h *Hub) ListUsers() ([]User, error) { return h.repo.ListUsers() }

// RemoveUser deletes the user with the given login.
func (h *Hub) RemoveUser(login string) error { return h.repo.RemoveUser(login) }
