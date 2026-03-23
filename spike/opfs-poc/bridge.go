//go:build js

package main

import (
	"errors"
	"sync"
	"sync/atomic"
	"syscall/js"
)

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

func goCoResolve(_ js.Value, args []js.Value) any {
	id := int64(args[0].Int())
	v, ok := coPending.LoadAndDelete(id)
	if !ok {
		return nil
	}
	ch := v.(chan coResult)
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

// coCall invokes a JS function with (id, ...args) and blocks until resolved.
func coCall(fn string, args ...any) (string, error) {
	id := coNextID.Add(1)
	ch := make(chan coResult, 1)
	coPending.Store(id, ch)
	callArgs := make([]any, 0, len(args)+1)
	callArgs = append(callArgs, id)
	callArgs = append(callArgs, args...)
	js.Global().Call(fn, callArgs...)
	res := <-ch
	return res.data, res.err
}

// toJSUint8Array converts Go []byte to a JS Uint8Array.
func toJSUint8Array(data []byte) js.Value {
	arr := js.Global().Get("Uint8Array").New(len(data))
	js.CopyBytesToJS(arr, data)
	return arr
}
