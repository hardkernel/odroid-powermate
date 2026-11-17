// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

package main

import (
	"flag"
	"fmt"
	"os"
)

func main() {
	if err := run(); err != nil {
		fmt.Fprintln(os.Stderr, "pmtui:", err)
		os.Exit(1)
	}
}

func run() error {
	host := flag.String("host", "192.168.4.1", "PowerMate host, optionally including http:// or https://")
	id := flag.String("id", "admin", "PowerMate login username")
	uart := flag.Bool("uart", false, "Open the UART terminal after login")
	fingerprint := flag.String("fingerprint", "",
		"pin the -host PowerMate's HTTPS certificate: its SHA-256 fingerprint (Settings > TLS); any other is refused")
	flag.Parse()

	pin, err := NormalizeFingerprint(*fingerprint)
	if err != nil {
		return fmt.Errorf("-fingerprint: %w", err)
	}
	return newTUI(*host, *id, pin, *uart).Run()
}
