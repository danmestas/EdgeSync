//go:build js

package wsdialer

import (
	"bytes"
	"errors"
	"fmt"
	"net"
	"sync"
	"syscall/js"
	"time"
)

// wsConn wraps a browser WebSocket as a net.Conn.
// Messages from the WebSocket are buffered in a channel; Read() drains
// them through a bytes.Reader to provide stream semantics.
//
// Note: js.FuncOf callbacks are not released — acceptable for spike scope.
// Production code should store them on the struct and Release() in Close().
type wsConn struct {
	ws     js.Value
	msgCh  chan []byte
	reader *bytes.Reader

	mu     sync.Mutex
	closed bool

	openCh chan struct{}
	errCh  chan error
}

func newWSConn(url string) *wsConn {
	c := &wsConn{
		msgCh:  make(chan []byte, 256),
		reader: bytes.NewReader(nil),
		openCh: make(chan struct{}),
		errCh:  make(chan error, 1),
	}

	ws := js.Global().Get("WebSocket").New(url)
	ws.Set("binaryType", "arraybuffer")
	c.ws = ws

	ws.Call("addEventListener", "open", js.FuncOf(func(_ js.Value, _ []js.Value) any {
		close(c.openCh)
		return nil
	}))

	ws.Call("addEventListener", "error", js.FuncOf(func(_ js.Value, args []js.Value) any {
		select {
		case c.errCh <- fmt.Errorf("websocket error"):
		default:
		}
		return nil
	}))

	ws.Call("addEventListener", "close", js.FuncOf(func(_ js.Value, _ []js.Value) any {
		c.mu.Lock()
		c.closed = true
		c.mu.Unlock()
		// Unblock any pending Read.
		select {
		case c.msgCh <- nil:
		default:
		}
		return nil
	}))

	ws.Call("addEventListener", "message", js.FuncOf(func(_ js.Value, args []js.Value) any {
		arrayBuf := args[0].Get("data")
		buf := make([]byte, arrayBuf.Get("byteLength").Int())
		js.CopyBytesToGo(buf, js.Global().Get("Uint8Array").New(arrayBuf))
		// Drop-newest if buffer full.
		select {
		case c.msgCh <- buf:
		default:
		}
		return nil
	}))

	return c
}

// waitOpen blocks until the WebSocket is open or fails.
func (c *wsConn) waitOpen() error {
	select {
	case <-c.openCh:
		return nil
	case err := <-c.errCh:
		return err
	case <-time.After(5 * time.Second):
		return errors.New("websocket open timeout (5s)")
	}
}

// Read implements net.Conn. Uses an internal bytes.Reader to bridge
// WebSocket message framing to stream semantics.
func (c *wsConn) Read(p []byte) (int, error) {
	// Drain leftover bytes from the previous message first.
	if c.reader.Len() > 0 {
		return c.reader.Read(p)
	}

	c.mu.Lock()
	closed := c.closed
	c.mu.Unlock()
	if closed {
		return 0, net.ErrClosed
	}

	msg, ok := <-c.msgCh
	if !ok || msg == nil {
		return 0, net.ErrClosed
	}
	c.reader.Reset(msg)
	return c.reader.Read(p)
}

// Write implements net.Conn. Sends binary data via WebSocket.
func (c *wsConn) Write(p []byte) (int, error) {
	c.mu.Lock()
	closed := c.closed
	c.mu.Unlock()
	if closed {
		return 0, net.ErrClosed
	}

	buf := js.Global().Get("Uint8Array").New(len(p))
	js.CopyBytesToJS(buf, p)
	c.ws.Call("send", buf.Get("buffer"))
	return len(p), nil
}

// Close implements net.Conn.
func (c *wsConn) Close() error {
	c.mu.Lock()
	defer c.mu.Unlock()
	if c.closed {
		return nil
	}
	c.closed = true
	c.ws.Call("close")
	return nil
}

// Stubs — nats.go uses its own ping/pong timeouts, not net.Conn deadlines.
func (c *wsConn) LocalAddr() net.Addr                { return wsAddr{} }
func (c *wsConn) RemoteAddr() net.Addr               { return wsAddr{} }
func (c *wsConn) SetDeadline(_ time.Time) error      { return nil }
func (c *wsConn) SetReadDeadline(_ time.Time) error  { return nil }
func (c *wsConn) SetWriteDeadline(_ time.Time) error { return nil }

type wsAddr struct{}

func (wsAddr) Network() string { return "websocket" }
func (wsAddr) String() string  { return "websocket" }
