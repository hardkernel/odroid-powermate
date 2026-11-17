// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

package main

import (
	"context"
	"strings"
	"testing"
	"time"

	"github.com/hardkernel/odroid-powermate/example/claude-plugin/mcp-server/internal/powermate"
	"github.com/modelcontextprotocol/go-sdk/mcp"
)

func TestUseDeviceWithoutAddress(t *testing.T) {
	m := &manager{root: context.Background(), stateDir: t.TempDir()}
	reached := ""
	next := func(_ context.Context, method string, req mcp.Request) (mcp.Result, error) {
		reached = req.(*mcp.CallToolRequest).Params.Name
		return &mcp.CallToolResult{}, nil
	}
	h := useDevice(m)(next)
	call := func(name string) *mcp.CallToolResult {
		res, err := h(context.Background(), "tools/call", &mcp.CallToolRequest{Params: &mcp.CallToolParamsRaw{Name: name}})
		if err != nil {
			t.Fatal(err)
		}
		return res.(*mcp.CallToolResult)
	}

	reached = ""
	if res := call("sbc_status"); !res.IsError || reached != "" ||
		!strings.Contains(res.Content[0].(*mcp.TextContent).Text, "powermate_discover") {
		t.Fatalf("sbc_status without an address: %+v, reached %q", res, reached)
	}
	if res := call("powermate_connect"); res.IsError || reached != "powermate_connect" {
		t.Fatalf("powermate_connect blocked without an address")
	}
	if res := call("powermate_discover"); res.IsError || reached != "powermate_discover" {
		t.Fatalf("powermate_discover blocked without an address")
	}
	if _, err := m.connect(context.Background(), "", true); err == nil {
		t.Fatal("empty host accepted")
	}
}

func TestDeviceClosesStreamsWhenIdle(t *testing.T) {
	client, err := powermate.NewClient("127.0.0.1:1", "admin", "password", nil)
	if err != nil {
		t.Fatal(err)
	}
	ctx, cancel := context.WithCancel(context.Background())
	defer cancel()
	d := newDevice(ctx, client, 200*time.Millisecond)
	open := func() bool {
		d.mu.Lock()
		defer d.mu.Unlock()
		return d.streamCancel != nil
	}

	if open() {
		t.Fatal("streams opened before any tool call")
	}
	short, stop := context.WithTimeout(ctx, 10*time.Millisecond)
	d.acquire(short)
	stop()
	time.Sleep(400 * time.Millisecond)
	if !open() {
		t.Fatal("streams closed while a tool call was running")
	}
	d.release()
	time.Sleep(400 * time.Millisecond)
	if open() {
		t.Fatal("streams still open after the idle timeout")
	}

	short, stop = context.WithTimeout(ctx, 10*time.Millisecond)
	d.acquire(short)
	stop()
	d.release()
	if !open() || len(d.eventsSince(time.Time{})) != 1 {
		t.Fatalf("reopen: open=%v events=%v", open(), d.eventsSince(time.Time{}))
	}
}
