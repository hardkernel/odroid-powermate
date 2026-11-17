// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

package main

import (
	"encoding/json"
	"net"
	"os"
	"path/filepath"
	"strings"
)

// Certificate fingerprints the user confirmed, per PowerMate (address without scheme or
// port), in ~/.config/pmtui/known_certs.json.

func certsPath() (string, error) {
	dir, err := os.UserConfigDir()
	if err != nil {
		return "", err
	}
	return filepath.Join(dir, "pmtui", "known_certs.json"), nil
}

func hostKey(host string) string {
	if i := strings.Index(host, "://"); i >= 0 {
		host = host[i+3:]
	}
	host = strings.TrimRight(host, "/")
	if h, _, err := net.SplitHostPort(host); err == nil {
		return h
	}
	return host
}

func loadKnownCerts() map[string]string {
	known := map[string]string{}
	path, err := certsPath()
	if err != nil {
		return known
	}
	if data, err := os.ReadFile(path); err == nil {
		json.Unmarshal(data, &known)
	}
	return known
}

func knownPin(host string) string {
	return loadKnownCerts()[hostKey(host)]
}

func saveKnownPin(host, pin string) error {
	path, err := certsPath()
	if err != nil {
		return err
	}
	known := loadKnownCerts()
	known[hostKey(host)] = pin
	data, err := json.MarshalIndent(known, "", "  ")
	if err != nil {
		return err
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o700); err != nil {
		return err
	}
	return os.WriteFile(path, append(data, '\n'), 0o600)
}
