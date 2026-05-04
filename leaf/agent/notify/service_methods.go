package notify

import (
	"context"
)

// ListThreads returns the threads for project from the agent's notify repo.
// Thin delegation to the libfossil-typed free function ListThreads.
//
// project is required because the notify repo can hold threads for multiple
// projects; the free-function shape requires it. A Service that tracks a
// default project would be a reasonable follow-up but is out of scope.
func (s *Service) ListThreads(ctx context.Context, project string) ([]ThreadSummary, error) {
	return ListThreads(s.repo, project)
}

// ReadThread returns the messages in (project, threadShort) ordered as
// stored. Thin delegation to ReadThread.
func (s *Service) ReadThread(ctx context.Context, project, threadShort string) ([]Message, error) {
	return ReadThread(s.repo, project, threadShort)
}

// MessageCount returns the number of messages in (project, threadShort).
func (s *Service) MessageCount(ctx context.Context, project, threadShort string) (int, error) {
	msgs, err := ReadThread(s.repo, project, threadShort)
	if err != nil {
		return 0, err
	}
	return len(msgs), nil
}

// ListDevices returns the registered devices on the agent's notify repo.
func (s *Service) ListDevices(ctx context.Context) ([]Device, error) {
	return ListDevices(s.repo)
}

// AddDevice registers a device on the agent's notify repo.
func (s *Service) AddDevice(ctx context.Context, dev Device) error {
	return AddDevice(s.repo, dev)
}

// RemoveDevice removes a device by name from the agent's notify repo.
func (s *Service) RemoveDevice(ctx context.Context, name string) error {
	return RemoveDevice(s.repo, name)
}

// CreatePairingToken generates a pairing token for name and stores it in
// the agent's notify repo.
func (s *Service) CreatePairingToken(ctx context.Context, name string) (string, error) {
	return CreatePairingToken(s.repo, name)
}

// ValidateToken validates a formatted pairing token against the agent's
// notify repo and returns the matching pending token (or error if none).
func (s *Service) ValidateToken(ctx context.Context, token string) (PendingToken, error) {
	return ValidateToken(s.repo, token)
}
