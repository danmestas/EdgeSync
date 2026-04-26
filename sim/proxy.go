package sim

import (
	"net"
	"sync"
	"sync/atomic"
	"time"
)

type FaultProxy struct {
	listener   net.Listener
	upstream   string
	mu         sync.Mutex
	latency    time.Duration
	partitions map[string]bool
	leafByAddr map[string]string
	closed     atomic.Bool
	conns      map[net.Conn]struct{}
}

func NewFaultProxy(upstream string) (*FaultProxy, error) {
	l, err := net.Listen("tcp", "127.0.0.1:0")
	if err != nil {
		return nil, err
	}
	p := &FaultProxy{
		listener:   l,
		upstream:   upstream,
		partitions: make(map[string]bool),
		leafByAddr: make(map[string]string),
		conns:      make(map[net.Conn]struct{}),
	}
	go p.acceptLoop()
	return p, nil
}

func (p *FaultProxy) Addr() string { return p.listener.Addr().String() }
func (p *FaultProxy) URL() string  { return "nats://" + p.Addr() }

func (p *FaultProxy) RegisterLeaf(remoteAddr, label string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.leafByAddr[remoteAddr] = label
}

func (p *FaultProxy) SetLatency(d time.Duration) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.latency = d
}

func (p *FaultProxy) Partition(target string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.partitions[target] = true
}

func (p *FaultProxy) Heal(target string) {
	p.mu.Lock()
	defer p.mu.Unlock()
	delete(p.partitions, target)
}

func (p *FaultProxy) HealAll() {
	p.mu.Lock()
	defer p.mu.Unlock()
	p.partitions = make(map[string]bool)
}

func (p *FaultProxy) DropConnections() {
	p.mu.Lock()
	conns := p.conns
	p.conns = make(map[net.Conn]struct{})
	p.mu.Unlock()
	for c := range conns {
		c.Close()
	}
}

func (p *FaultProxy) Close() error {
	p.closed.Store(true)
	p.DropConnections()
	return p.listener.Close()
}

func (p *FaultProxy) acceptLoop() {
	for {
		clientConn, err := p.listener.Accept()
		if err != nil {
			if p.closed.Load() {
				return
			}
			continue
		}
		go p.handleConn(clientConn)
	}
}

func (p *FaultProxy) handleConn(client net.Conn) {
	p.mu.Lock()
	p.conns[client] = struct{}{}
	p.mu.Unlock()

	defer func() {
		client.Close()
		p.mu.Lock()
		delete(p.conns, client)
		p.mu.Unlock()
	}()

	p.mu.Lock()
	partitioned := p.partitions["*"]
	if !partitioned {
		if label, ok := p.leafByAddr[client.RemoteAddr().String()]; ok {
			partitioned = p.partitions[label]
		}
	}
	p.mu.Unlock()
	if partitioned {
		return
	}

	upstream, err := net.Dial("tcp", p.upstream)
	if err != nil {
		return
	}

	p.mu.Lock()
	p.conns[upstream] = struct{}{}
	p.mu.Unlock()

	defer func() {
		upstream.Close()
		p.mu.Lock()
		delete(p.conns, upstream)
		p.mu.Unlock()
	}()

	done := make(chan struct{}, 2)
	forward := func(dst, src net.Conn) {
		defer func() { done <- struct{}{} }()
		buf := make([]byte, 32*1024)
		for {
			n, err := src.Read(buf)
			if n > 0 {
				p.mu.Lock()
				lat := p.latency
				part := p.partitions["*"]
				p.mu.Unlock()

				if part {
					return
				}
				if lat > 0 {
					time.Sleep(lat)
				}
				if _, werr := dst.Write(buf[:n]); werr != nil {
					return
				}
			}
			if err != nil {
				return
			}
		}
	}
	go forward(upstream, client)
	go forward(client, upstream)
	<-done
}
