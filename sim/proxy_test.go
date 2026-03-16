package sim

import (
	"net"
	"testing"
	"time"
)

func TestFaultProxyForwards(t *testing.T) {
	echoL, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		t.Fatal(err)
	}
	defer echoL.Close()
	go func() {
		for {
			c, err := echoL.Accept()
			if err != nil {
				return
			}
			go func() {
				defer c.Close()
				buf := make([]byte, 1024)
				n, _ := c.Read(buf)
				c.Write(buf[:n])
			}()
		}
	}()

	proxy, err := NewFaultProxy(echoL.Addr().String())
	if err != nil {
		t.Fatal(err)
	}
	defer proxy.Close()

	conn, err := net.Dial("tcp", proxy.Addr())
	if err != nil {
		t.Fatal(err)
	}
	defer conn.Close()

	conn.Write([]byte("hello"))
	conn.SetReadDeadline(time.Now().Add(2 * time.Second))
	buf := make([]byte, 1024)
	n, err := conn.Read(buf)
	if err != nil {
		t.Fatal(err)
	}
	if string(buf[:n]) != "hello" {
		t.Fatalf("got %q, want %q", buf[:n], "hello")
	}
}

func TestFaultProxyPartition(t *testing.T) {
	echoL, _ := net.Listen("tcp", "127.0.0.1:0")
	defer echoL.Close()
	go func() {
		for {
			c, err := echoL.Accept()
			if err != nil {
				return
			}
			go func() {
				defer c.Close()
				buf := make([]byte, 1024)
				n, _ := c.Read(buf)
				c.Write(buf[:n])
			}()
		}
	}()

	proxy, _ := NewFaultProxy(echoL.Addr().String())
	defer proxy.Close()

	proxy.Partition("*")

	conn, err := net.DialTimeout("tcp", proxy.Addr(), 1*time.Second)
	if err != nil {
		return // Connection refused — partition working
	}
	defer conn.Close()

	conn.Write([]byte("hello"))
	conn.SetReadDeadline(time.Now().Add(1 * time.Second))
	buf := make([]byte, 1024)
	_, err = conn.Read(buf)
	if err == nil {
		t.Fatal("expected read to fail during partition")
	}
}
