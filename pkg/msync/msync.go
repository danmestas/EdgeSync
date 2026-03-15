// Package msync provides NATS messaging for Fossil artifact sync.
//
// Subject namespace:
//
//	fossil.<project-code>.igot    "I have this artifact"
//	fossil.<project-code>.gimme   "I need this artifact"
//	fossil.<project-code>.file    Artifact payload delivery
//	fossil.<project-code>.delta   Delta-compressed artifact
//	fossil.<project-code>.meta    Config, tickets, wiki changes
//	fossil.<project-code>.events  Commit notifications
package msync

import (
	"fmt"
)

// Subject returns the full NATS subject for a project and message type.
func Subject(projectCode, msgType string) string {
	return fmt.Sprintf("fossil.%s.%s", projectCode, msgType)
}

// SubjectPrefix returns the wildcard subject for all messages in a project.
func SubjectPrefix(projectCode string) string {
	return fmt.Sprintf("fossil.%s.>", projectCode)
}

// StreamName returns the JetStream stream name for a project.
func StreamName(projectCode string) string {
	return fmt.Sprintf("FOSSIL_%s", projectCode)
}
