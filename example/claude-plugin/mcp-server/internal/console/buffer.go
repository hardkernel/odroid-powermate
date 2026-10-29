// Package console buffers the target's UART stream so tools can read, wait for and
// match output that arrived while no tool call was running.
package console

import (
	"context"
	"sync"
	"time"
)

// Buffer is a bounded byte log addressed by absolute cursors. A cursor counts every
// byte received since the process started, so it stays valid after old data is
// discarded; reads from a discarded cursor report how much was lost.
type Buffer struct {
	mu        sync.Mutex
	data      []byte
	start     int64 // absolute cursor of data[0]
	limit     int
	changed   chan struct{}
	lastWrite time.Time
}

func NewBuffer(limit int) *Buffer {
	return &Buffer{limit: limit, changed: make(chan struct{})}
}

// Write appends p and wakes every waiter.
func (b *Buffer) Write(p []byte) {
	if len(p) == 0 {
		return
	}
	b.mu.Lock()
	b.data = append(b.data, p...)
	if over := len(b.data) - b.limit; over > 0 {
		// Compact instead of reslicing so the backing array does not grow forever.
		b.data = append(b.data[:0], b.data[over:]...)
		b.start += int64(over)
	}
	b.lastWrite = time.Now()
	close(b.changed)
	b.changed = make(chan struct{})
	b.mu.Unlock()
}

// End returns the cursor just past the newest byte.
func (b *Buffer) End() int64 {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.start + int64(len(b.data))
}

// LastWrite returns when data last arrived (zero if never).
func (b *Buffer) LastWrite() time.Time {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.lastWrite
}

// Snapshot is a copy of the bytes from From up to End.
type Snapshot struct {
	Data []byte
	From int64
	End  int64
	// Lost is how many bytes between the requested cursor and From were discarded.
	Lost int64
}

// Since copies everything from cursor to the end. A cursor past the end is clamped.
func (b *Buffer) Since(cursor int64) Snapshot {
	b.mu.Lock()
	defer b.mu.Unlock()
	return b.sinceLocked(cursor)
}

func (b *Buffer) sinceLocked(cursor int64) Snapshot {
	end := b.start + int64(len(b.data))
	snap := Snapshot{From: cursor, End: end}
	if cursor < b.start {
		snap.Lost = b.start - cursor
		snap.From = b.start
	}
	if snap.From > end {
		snap.From = end
	}
	snap.Data = append([]byte(nil), b.data[snap.From-b.start:]...)
	return snap
}

// Wait calls check with the data since cursor each time new data arrives, until check
// returns true, ctx ends, or timeout passes. It reports whether check succeeded and
// the last snapshot it was given.
func (b *Buffer) Wait(ctx context.Context, cursor int64, timeout time.Duration, check func(Snapshot) bool) (bool, Snapshot) {
	deadline := time.NewTimer(timeout)
	defer deadline.Stop()
	for {
		b.mu.Lock()
		snap := b.sinceLocked(cursor)
		changed := b.changed
		b.mu.Unlock()

		if check(snap) {
			return true, snap
		}
		select {
		case <-changed:
		case <-deadline.C:
			return false, snap
		case <-ctx.Done():
			return false, snap
		}
	}
}

// WaitQuiet blocks until no data has arrived for quiet, or until max passes.
func (b *Buffer) WaitQuiet(ctx context.Context, quiet, max time.Duration) {
	deadline := time.Now().Add(max)
	for {
		idle := time.Since(b.LastWrite())
		if idle >= quiet || time.Now().After(deadline) {
			return
		}
		wait := min(quiet-idle, time.Until(deadline))
		select {
		case <-time.After(wait):
		case <-ctx.Done():
			return
		}
	}
}
