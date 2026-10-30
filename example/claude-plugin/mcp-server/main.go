// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

// powermate-mcp is an MCP stdio server that lets an agent operate an SBC through an
// ODROID PowerMate: power outputs, power/reset header pins, the UART console and
// the power sensors.
//
// Configuration comes from the environment:
//
//	POWERMATE_HOST      PowerMate address; when unset, the address saved by
//	                    powermate_connect. With neither, every tool but
//	                    powermate_connect asks the agent to get the address from the user.
//	POWERMATE_USERNAME  web login (default "admin")
//	POWERMATE_PASSWORD  web login (default "password")
//	POWERMATE_STATE_DIR token cache and capture logs (default: user cache dir)
package main

import (
	"context"
	"flag"
	"fmt"
	"io"
	"log"
	"os"
	"os/signal"
	"path/filepath"
	"strings"
	"syscall"

	"github.com/modelcontextprotocol/go-sdk/mcp"
)

var version = "dev"

// fileTokenStore keeps the token across restarts so each new MCP session reuses it
// instead of taking another of the firmware's four token slots.
type fileTokenStore struct{ path string }

func (s fileTokenStore) Load() string {
	data, err := os.ReadFile(s.path)
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(data))
}

func (s fileTokenStore) Save(token string) {
	if err := os.MkdirAll(filepath.Dir(s.path), 0o700); err != nil {
		log.Printf("token cache: %v", err)
		return
	}
	if err := os.WriteFile(s.path, []byte(token+"\n"), 0o600); err != nil {
		log.Printf("token cache: %v", err)
	}
}

func getenv(key, def string) string {
	// An unexpanded "${...}" means the launcher had no value to substitute.
	if v := strings.TrimSpace(os.Getenv(key)); v != "" && !strings.HasPrefix(v, "${") {
		return v
	}
	return def
}

func main() {
	showVersion := flag.Bool("version", false, "print version and exit")
	flag.Parse()
	if *showVersion {
		fmt.Println(version)
		return
	}

	// stdout carries the MCP protocol; everything else goes to stderr.
	log.SetOutput(os.Stderr)
	log.SetFlags(log.LstdFlags | log.Lmsgprefix)
	log.SetPrefix("powermate-mcp: ")

	stateDir := getenv("POWERMATE_STATE_DIR", "")
	if stateDir == "" {
		cache, err := os.UserCacheDir()
		if err != nil {
			cache = os.TempDir()
		}
		stateDir = filepath.Join(cache, "powermate-mcp")
	}

	ctx, stop := signal.NotifyContext(context.Background(), os.Interrupt, syscall.SIGTERM)
	defer stop()

	m := &manager{
		root:     ctx,
		stateDir: stateDir,
		username: getenv("POWERMATE_USERNAME", "admin"),
		password: getenv("POWERMATE_PASSWORD", "password"),
	}
	// An explicit POWERMATE_HOST wins; otherwise use the address saved by
	// powermate_connect. There is no default: a guessed address would point the
	// tools at whatever answers there.
	host, source := getenv("POWERMATE_HOST", ""), "env"
	if host == "" {
		host, source = m.savedHost(), "saved"
	}
	if host != "" {
		if err := m.start(host, source); err != nil {
			log.Fatal(err)
		}
	}

	server := mcp.NewServer(&mcp.Implementation{Name: "powermate", Version: version}, &mcp.ServerOptions{
		Instructions: "sbc_* tools operate one SBC wired to an ODROID PowerMate; powermate_* tools " +
			"configure the PowerMate itself (network, limits, options) and preview their changes and risks " +
			"unless confirm=true. sbc_power off/cycle is a hard power cut. Console output is buffered " +
			"continuously; use cursors from one tool as 'since' in the next to avoid missing output. " +
			"If no PowerMate address is configured, ask the user for it and call powermate_connect; never guess one.",
	})
	server.AddReceivingMiddleware(requireHost(m))
	t := &tools{m: m, logDir: filepath.Join(stateDir, "logs")}
	registerTools(server, t)
	registerAdminTools(server, t)

	if err := server.Run(ctx, &mcp.StdioTransport{}); err != nil && ctx.Err() == nil && err != io.EOF {
		log.Fatal(err)
	}
}

const noHostMsg = "No PowerMate address is configured. Ask the user for the PowerMate's IP address or host " +
	"name (shown in its web UI, the router's DHCP list, or wifi_status on its USB debug console) and call " +
	"powermate_connect(host=...); do not guess one. A factory-fresh PowerMate is not on any network yet: the user " +
	"first joins it to Wi-Fi from a phone through its setup AP (powermate / 192.168.4.1)."

// requireHost answers every tool call except powermate_connect with noHostMsg until
// an address is known.
func requireHost(m *manager) mcp.Middleware {
	return func(next mcp.MethodHandler) mcp.MethodHandler {
		return func(ctx context.Context, method string, req mcp.Request) (mcp.Result, error) {
			if call, ok := req.(*mcp.CallToolRequest); ok && call.Params.Name != "powermate_connect" && m.current() == nil {
				return &mcp.CallToolResult{IsError: true, Content: []mcp.Content{&mcp.TextContent{Text: noHostMsg}}}, nil
			}
			return next(ctx, method, req)
		}
	}
}

func sanitize(s string) string {
	return strings.Map(func(r rune) rune {
		if r >= 'a' && r <= 'z' || r >= 'A' && r <= 'Z' || r >= '0' && r <= '9' || r == '.' || r == '-' {
			return r
		}
		return '_'
	}, s)
}
