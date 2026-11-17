// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

package powermate

import (
	"context"
	"errors"
	"net/http"
	"net/http/httptest"
	"net/url"
	"strings"
	"sync/atomic"
	"testing"
	"time"

	"github.com/gorilla/websocket"
)

// fakeDevice answers like the firmware: /login hands out a token, /api/version needs it.
func fakeDevice(requests *atomic.Int32) http.Handler {
	return http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		requests.Add(1)
		switch r.URL.Path {
		case "/login":
			w.Write([]byte(`{"token":"t0k3n"}`))
		case "/api/version":
			if r.Header.Get("Authorization") != "Bearer t0k3n" {
				http.Error(w, "Authorization token required", http.StatusUnauthorized)
				return
			}
			w.Write([]byte(`{"version":"v-test"}`))
		default:
			http.NotFound(w, r)
		}
	})
}

func ctx(t *testing.T) context.Context {
	c, cancel := context.WithTimeout(context.Background(), 5*time.Second)
	t.Cleanup(cancel)
	return c
}

func TestTLSUnpinnedAccepted(t *testing.T) {
	var requests atomic.Int32
	srv := httptest.NewTLSServer(fakeDevice(&requests))
	defer srv.Close()

	c, err := NewClient(srv.URL, "admin", "secret", nil, "")
	if err != nil {
		t.Fatal(err)
	}
	if v, err := c.GetVersion(ctx(t)); err != nil || v.Version != "v-test" {
		t.Fatalf("got %+v, %v; a self-signed certificate should be accepted without a pin", v, err)
	}
}

func TestTLSPinned(t *testing.T) {
	var requests atomic.Int32
	srv := httptest.NewTLSServer(fakeDevice(&requests))
	defer srv.Close()
	fp := FormatFingerprint(certFingerprint(srv.Certificate().Raw)) // as the web UI shows it

	c, err := NewClient(srv.URL, "admin", "secret", nil, strings.ToLower(fp))
	if err != nil {
		t.Fatal(err)
	}
	if v, err := c.GetVersion(ctx(t)); err != nil || v.Version != "v-test" {
		t.Fatalf("got %+v, %v", v, err)
	}

	// A new connection resumes the TLS session instead of a full handshake.
	c.CloseIdleConnections()
	request, _ := http.NewRequestWithContext(ctx(t), http.MethodGet, srv.URL+"/api/version", nil)
	response, err := c.httpClient.Do(request)
	if err != nil {
		t.Fatal(err)
	}
	response.Body.Close()
	if response.TLS == nil || !response.TLS.DidResume {
		t.Fatalf("second connection did not resume the TLS session")
	}
}

func TestTLSWrongPin(t *testing.T) {
	var requests atomic.Int32
	srv := httptest.NewTLSServer(fakeDevice(&requests))
	defer srv.Close()

	c, err := NewClient(srv.URL, "admin", "secret", nil, strings.Repeat("ab", 32))
	if err != nil {
		t.Fatal(err)
	}
	_, err = c.GetVersion(ctx(t))
	var certErr *CertError
	if !errors.As(err, &certErr) || certErr.Pinned == "" {
		t.Fatalf("got %v, want a changed-certificate CertError", err)
	}
	if n := requests.Load(); n != 0 {
		t.Fatalf("the server saw %d requests with a wrong certificate", n)
	}
}

func TestBadFingerprint(t *testing.T) {
	if _, err := NewClient("10.0.0.1", "a", "b", nil, "12:34"); err == nil {
		t.Fatal("a short fingerprint was accepted")
	}
}

// With HTTPS on, the firmware's port 80 redirects every GET to https:// on the same
// host. The client must find that out without sending the password or a token over HTTP.
func TestResolveHTTPS(t *testing.T) {
	var requests atomic.Int32
	tlsSrv := httptest.NewTLSServer(fakeDevice(&requests))
	defer tlsSrv.Close()
	target, _ := url.Parse(tlsSrv.URL)

	var plainLeaks atomic.Int32
	plain := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		if r.Method != http.MethodGet || r.Header.Get("Authorization") != "" || r.URL.Path == "/login" {
			plainLeaks.Add(1)
		}
		http.Redirect(w, r, "https://"+target.Host+r.URL.RequestURI(), http.StatusTemporaryRedirect)
	}))
	defer plain.Close()

	c, err := NewClient(strings.TrimPrefix(plain.URL, "http://"), "admin", "secret", nil,
		certFingerprint(tlsSrv.Certificate().Raw))
	if err != nil {
		t.Fatal(err)
	}
	if v, err := c.GetVersion(ctx(t)); err != nil || v.Version != "v-test" {
		t.Fatalf("got %+v, %v", v, err)
	}
	if n := plainLeaks.Load(); n != 0 {
		t.Fatalf("%d requests with credentials went to the plain HTTP port", n)
	}
	if !strings.HasPrefix(c.URL(), "https://") {
		t.Fatalf("URL %s, want https://", c.URL())
	}
}

func TestResolveRefusesOtherHost(t *testing.T) {
	plain := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, "https://elsewhere.example/", http.StatusTemporaryRedirect)
	}))
	defer plain.Close()
	c, _ := NewClient(plain.URL, "admin", "secret", nil, "")
	if _, err := c.GetVersion(ctx(t)); err == nil || !strings.Contains(err.Error(), "same host") {
		t.Fatalf("got %v, want the redirect to another host refused", err)
	}
}

// An https:// device that redirects back to http:// is not followed.
func TestNoDowngradeRedirect(t *testing.T) {
	var plainRequests atomic.Int32
	plain := httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		plainRequests.Add(1)
	}))
	defer plain.Close()
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		http.Redirect(w, r, plain.URL+r.URL.RequestURI(), http.StatusTemporaryRedirect)
	}))
	defer srv.Close()
	c, _ := NewClient(srv.URL, "admin", "secret", nil, certFingerprint(srv.Certificate().Raw))
	if _, err := c.GetVersion(ctx(t)); err == nil {
		t.Fatal("a redirect from https:// to http:// was followed")
	}
	if n := plainRequests.Load(); n != 0 {
		t.Fatalf("%d requests reached the http:// server", n)
	}
}

func TestWebSocketUsesPin(t *testing.T) {
	upgrader := websocket.Upgrader{}
	srv := httptest.NewTLSServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		conn, err := upgrader.Upgrade(w, r, nil)
		if err == nil {
			conn.Close()
		}
	}))
	defer srv.Close()

	pinned, _ := NewClient(srv.URL, "admin", "secret", nil, certFingerprint(srv.Certificate().Raw))
	conn, _, err := pinned.dialWebSocket(ctx(t), "/ws", "t")
	if err != nil {
		t.Fatalf("pinned wss:// dial failed: %v", err)
	}
	conn.Close()

	wrong, _ := NewClient(srv.URL, "admin", "secret", nil, strings.Repeat("cd", 32))
	_, _, err = wrong.dialWebSocket(ctx(t), "/ws", "t")
	var certErr *CertError
	if !errors.As(err, &certErr) {
		t.Fatalf("wss:// with a wrong pin: got %v, want a CertError", err)
	}
}
