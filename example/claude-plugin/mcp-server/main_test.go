package main

import (
	"context"
	"strings"
	"testing"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

func TestRequireHostWithoutAddress(t *testing.T) {
	m := &manager{root: context.Background(), stateDir: t.TempDir()}
	reached := ""
	next := func(_ context.Context, method string, req mcp.Request) (mcp.Result, error) {
		reached = req.(*mcp.CallToolRequest).Params.Name
		return &mcp.CallToolResult{}, nil
	}
	h := requireHost(m)(next)
	call := func(name string) *mcp.CallToolResult {
		res, err := h(context.Background(), "tools/call", &mcp.CallToolRequest{Params: &mcp.CallToolParamsRaw{Name: name}})
		if err != nil {
			t.Fatal(err)
		}
		return res.(*mcp.CallToolResult)
	}

	reached = ""
	if res := call("sbc_status"); !res.IsError || reached != "" ||
		!strings.Contains(res.Content[0].(*mcp.TextContent).Text, "Ask the user") {
		t.Fatalf("sbc_status without an address: %+v, reached %q", res, reached)
	}
	if res := call("powermate_connect"); res.IsError || reached != "powermate_connect" {
		t.Fatalf("powermate_connect blocked without an address")
	}
	if _, err := m.connect(context.Background(), "", true); err == nil {
		t.Fatal("empty host accepted")
	}
}
