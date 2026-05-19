package hub

import "fmt"

// assert panics with msg when cond is false. TigerStyle-flavored runtime
// invariant check: programmer-bugs (e.g. an internal lifecycle invariant
// violated) surface loudly instead of as silent undefined behavior or a
// remote nil-pointer panic deeper in the stack.
//
// Use sparingly and only for invariants the package itself controls — never
// for input validation, which belongs on the public API surface as a typed
// error return.
func assert(cond bool, msg string) {
	if !cond {
		panic(fmt.Sprintf("hub: assertion failed: %s", msg))
	}
}
