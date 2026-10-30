#!/bin/sh
# SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
# SPDX-License-Identifier: Apache-2.0

# Launch powermate-mcp, building it first when the binary is missing or older than
# the sources. Build output goes to stderr because stdout carries the MCP protocol.
set -e
cd "$(dirname "$0")"
bin=bin/powermate-mcp
if [ ! -x "$bin" ] || [ -n "$(find . \( -name '*.go' -o -name go.mod -o -name go.sum \) -newer "$bin" | head -n 1)" ]; then
	if ! command -v go >/dev/null 2>&1; then
		echo "powermate-mcp: Go is required to build the server (see README.md)" >&2
		exit 1
	fi
	echo "powermate-mcp: building $bin" >&2
	go build -o "$bin" . >&2
fi
exec "$bin" "$@"
