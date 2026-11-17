// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

package main

import (
	"crypto/sha256"
	"crypto/subtle"
	"crypto/tls"
	"encoding/hex"
	"errors"
	"fmt"
	"net"
	"strings"
)

// A PowerMate with HTTPS on usually serves a certificate it created itself. Its
// certificate is accepted as it is, so HTTPS keeps the traffic private but does not prove
// which device answers. Pinning its SHA-256 fingerprint (the value in Settings > TLS of
// the PowerMate web UI) makes the client refuse any other certificate.

// CertError means the PowerMate's certificate is not the pinned one. Nothing (password
// or token) was sent.
type CertError struct {
	Host        string
	Fingerprint string // SHA-256 of the certificate the device presented, lowercase hex
	Pinned      string // the expected fingerprint
}

func (e *CertError) Error() string {
	return fmt.Sprintf("the certificate of %s is not the pinned one: it has SHA-256 fingerprint %s, the pinned one is %s",
		e.Host, FormatFingerprint(e.Fingerprint), FormatFingerprint(e.Pinned))
}

// NormalizeFingerprint accepts a SHA-256 fingerprint with or without colons or spaces,
// in either case, and returns it as 64 lowercase hex digits. An empty input stays empty.
func NormalizeFingerprint(s string) (string, error) {
	s = strings.NewReplacer(":", "", " ", "", "-", "").Replace(strings.TrimSpace(s))
	if s == "" {
		return "", nil
	}
	s = strings.ToLower(s)
	if b, err := hex.DecodeString(s); err != nil || len(b) != sha256.Size {
		return "", errors.New("a SHA-256 fingerprint is 64 hex digits (colons optional)")
	}
	return s, nil
}

// FormatFingerprint writes a fingerprint the way the PowerMate web UI shows it: AB:CD:...
func FormatFingerprint(fp string) string {
	var b strings.Builder
	for i := 0; i+1 < len(fp); i += 2 {
		if i > 0 {
			b.WriteByte(':')
		}
		b.WriteString(strings.ToUpper(fp[i : i+2]))
	}
	return b.String()
}

func certFingerprint(der []byte) string {
	sum := sha256.Sum256(der)
	return hex.EncodeToString(sum[:])
}

// newTLSConfig accepts any certificate, or only the pinned one when pin is set. The check
// is in VerifyConnection, which also runs on resumed sessions; the standard verification
// is off because it would refuse a self-signed certificate. Session tickets make
// reconnects take ~50 ms instead of ~0.7 s.
func newTLSConfig(host, pin string) *tls.Config {
	serverName := host
	if h, _, err := net.SplitHostPort(host); err == nil {
		serverName = h
	}
	return &tls.Config{
		MinVersion:         tls.VersionTLS12,
		InsecureSkipVerify: true, // replaced by VerifyConnection
		ClientSessionCache: tls.NewLRUClientSessionCache(8),
		VerifyConnection: func(cs tls.ConnectionState) error {
			if len(cs.PeerCertificates) == 0 {
				return errors.New("the PowerMate sent no certificate")
			}
			if pin == "" {
				return nil
			}
			fp := certFingerprint(cs.PeerCertificates[0].Raw)
			if subtle.ConstantTimeCompare([]byte(fp), []byte(pin)) == 1 {
				return nil
			}
			return &CertError{Host: serverName, Fingerprint: fp, Pinned: pin}
		},
	}
}
