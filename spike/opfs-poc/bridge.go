//go:build js

package main

import (
	"errors"
	"fmt"
	"sync"
	"sync/atomic"
	"syscall/js"
	"time"
)

// coCallTimeout is the maximum time coCall waits for JS to resolve.
// Prevents goroutine leaks if JS never calls _go_co_resolve.
const coCallTimeout = 30 * time.Second

var (
	coNextID  atomic.Int64
	coPending sync.Map // int64 → chan coResult
)

type coResult struct {
	data string
	err  error
}

func init() {
	js.Global().Set("_go_co_resolve", js.FuncOf(goCoResolve))
}

// goCoResolve is the JS callback that unblocks a waiting coCall goroutine.
func goCoResolve(_ js.Value, args []js.Value) any {
	if len(args) < 2 {
		panic("goCoResolve: expected at least 2 args (id, error)")
	}
	id := int64(args[0].Int())
	v, ok := coPending.LoadAndDelete(id)
	if !ok {
		return nil // Already timed out and cleaned up.
	}
	ch, ok := v.(chan coResult)
	if !ok {
		panic(fmt.Sprintf("goCoResolve: pending value for id=%d is not chan coResult", id))
	}
	if args[1].IsNull() || args[1].IsUndefined() {
		data := ""
		if len(args) > 2 && !args[2].IsNull() && !args[2].IsUndefined() {
			data = args[2].String()
		}
		ch <- coResult{data: data}
	} else {
		ch <- coResult{err: errors.New(args[1].String())}
	}
	return nil
}

// coCall invokes a JS function with (id, ...args) and blocks until resolved
// or the timeout expires.
func coCall(fn string, args ...any) (string, error) {
	if fn == "" {
		panic("coCall: fn must not be empty")
	}
	id := coNextID.Add(1)
	// Buffer of 1 so JS callback doesn't block if we've already timed out.
	ch := make(chan coResult, 1)
	coPending.Store(id, ch)

	callArgs := make([]any, 0, len(args)+1)
	callArgs = append(callArgs, id)
	callArgs = append(callArgs, args...)
	js.Global().Call(fn, callArgs...)

	select {
	case res := <-ch:
		return res.data, res.err
	case <-time.After(coCallTimeout):
		coPending.Delete(id)
		return "", fmt.Errorf("coCall %s: timed out after %s", fn, coCallTimeout)
	}
}

// toJSUint8Array converts Go []byte to a JS Uint8Array.
func toJSUint8Array(data []byte) js.Value {
	if data == nil {
		panic("toJSUint8Array: data must not be nil")
	}
	arr := js.Global().Get("Uint8Array").New(len(data))
	js.CopyBytesToJS(arr, data)
	return arr
}
