package notify

import (
	"encoding/json"
	"fmt"
	"time"

	libfossil "github.com/danmestas/libfossil"
)

const pendingTokensFilePath = "_notify/pending_tokens.json"

// PendingToken is a hashed pairing token awaiting validation.
type PendingToken struct {
	TokenHash  string    `json:"token_hash"`
	CreatedAt  time.Time `json:"created_at"`
	ExpiresAt  time.Time `json:"expires_at"`
	DeviceName string    `json:"device_name"`
}

// pendingTokenStore is the JSON structure stored in the notify repo.
type pendingTokenStore struct {
	Tokens []PendingToken `json:"tokens"`
}

// StorePendingToken adds a pending token to the repo. Prunes expired tokens.
func StorePendingToken(r *libfossil.Repo, pt PendingToken) error {
	tokens, err := readPendingTokens(r)
	if err != nil {
		return err
	}
	// Prune expired.
	now := time.Now().UTC()
	tokens = pruneExpired(tokens, now)
	tokens = append(tokens, pt)
	return commitPendingTokens(r, tokens)
}

// ValidateToken checks a raw token against pending tokens.
// On success, removes the token (single-use) and returns the PendingToken metadata.
// Returns error if token is invalid, expired, or already consumed.
func ValidateToken(r *libfossil.Repo, formattedToken string) (PendingToken, error) {
	tokens, err := readPendingTokens(r)
	if err != nil {
		return PendingToken{}, err
	}

	hash := HashToken(formattedToken)
	now := time.Now().UTC()

	for i, pt := range tokens {
		if pt.TokenHash != hash {
			continue
		}
		// Found — check expiry.
		if now.After(pt.ExpiresAt) {
			// Remove expired token and commit.
			tokens = append(tokens[:i], tokens[i+1:]...)
			_ = commitPendingTokens(r, tokens)
			return PendingToken{}, fmt.Errorf("token expired")
		}
		// Valid — remove (single-use) and commit.
		matched := pt
		tokens = append(tokens[:i], tokens[i+1:]...)
		if err := commitPendingTokens(r, tokens); err != nil {
			return PendingToken{}, err
		}
		return matched, nil
	}
	return PendingToken{}, fmt.Errorf("token not found or already consumed")
}

// CreatePairingToken generates a token, hashes it, stores the pending entry, and
// returns the display-formatted token (XXXX-XXXX-XXXX). This is the single entry
// point CLI commands should use.
func CreatePairingToken(r *libfossil.Repo, name string) (string, error) {
	tok, err := GenerateToken()
	if err != nil {
		return "", err
	}
	err = StorePendingToken(r, PendingToken{
		TokenHash:  HashToken(tok),
		DeviceName: name,
		CreatedAt:  time.Now().UTC(),
		ExpiresAt:  time.Now().UTC().Add(10 * time.Minute),
	})
	if err != nil {
		return "", err
	}
	return tok, nil
}

// readPendingTokens reads the pending token store from the repo.
func readPendingTokens(r *libfossil.Repo) ([]PendingToken, error) {
	data, err := readFileContent(r, pendingTokensFilePath)
	if err != nil {
		return nil, nil // file doesn't exist yet
	}
	var store pendingTokenStore
	if err := json.Unmarshal(data, &store); err != nil {
		return nil, fmt.Errorf("unmarshal pending tokens: %w", err)
	}
	return store.Tokens, nil
}

// commitPendingTokens serializes and commits the pending token list.
func commitPendingTokens(r *libfossil.Repo, tokens []PendingToken) error {
	store := pendingTokenStore{Tokens: tokens}
	data, err := json.MarshalIndent(store, "", "  ")
	if err != nil {
		return fmt.Errorf("marshal pending tokens: %w", err)
	}
	_, _, err = r.Commit(libfossil.CommitOpts{
		Files: []libfossil.FileToCommit{
			{Name: pendingTokensFilePath, Content: data},
		},
		Comment: "notify: update pending tokens",
		User:    "notify",
	})
	if err != nil {
		return fmt.Errorf("commit pending tokens: %w", err)
	}
	return nil
}

// pruneExpired removes tokens that have passed their expiry time.
func pruneExpired(tokens []PendingToken, now time.Time) []PendingToken {
	live := tokens[:0]
	for _, pt := range tokens {
		if now.Before(pt.ExpiresAt) {
			live = append(live, pt)
		}
	}
	return live
}
