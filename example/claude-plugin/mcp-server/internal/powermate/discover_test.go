// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

package powermate

import (
	"context"
	"net"
	"os"
	"testing"
	"time"

	"golang.org/x/net/dns/dnsmessage"
)

func mustName(t *testing.T, s string) dnsmessage.Name {
	t.Helper()
	n, err := dnsmessage.NewName(s)
	if err != nil {
		t.Fatal(err)
	}
	return n
}

// reply builds a PowerMate's answer the way ESP-IDF mdns sends it.
func reply(t *testing.T, suffix string, withA bool) []byte {
	return replyWith(t, suffix, withA, 80, []string{"fw=v1.4.0-test"})
}

func replyWith(t *testing.T, suffix string, withA bool, port uint16, txt []string) []byte {
	instance := mustName(t, "ODROID PowerMate "+suffix+"."+powermateService)
	host := mustName(t, "odroid-pm-"+suffix+".local.")
	b := dnsmessage.NewBuilder(nil, dnsmessage.Header{Response: true, Authoritative: true})
	b.EnableCompression()
	b.StartAnswers()
	hdr := func(n dnsmessage.Name) dnsmessage.ResourceHeader {
		return dnsmessage.ResourceHeader{Name: n, Class: dnsmessage.ClassINET, TTL: 120}
	}
	b.PTRResource(hdr(mustName(t, powermateService)), dnsmessage.PTRResource{PTR: instance})
	b.StartAdditionals()
	b.SRVResource(hdr(instance), dnsmessage.SRVResource{Port: port, Target: host})
	b.TXTResource(hdr(instance), dnsmessage.TXTResource{TXT: txt})
	if withA {
		b.AResource(hdr(host), dnsmessage.AResource{A: [4]byte{192, 168, 30, 4}})
	}
	packet, err := b.Finish()
	if err != nil {
		t.Fatal(err)
	}
	return packet
}

func TestParseAnswer(t *testing.T) {
	found := map[string]*Discovered{}
	parseAnswer(reply(t, "b0f4", true), &net.UDPAddr{IP: net.IPv4(192, 168, 30, 4)}, found)
	parseAnswer(reply(t, "1a2b", false), &net.UDPAddr{IP: net.IPv4(192, 168, 30, 9)}, found)
	if len(found) != 2 {
		t.Fatalf("found %d PowerMates, want 2", len(found))
	}
	got := found["ODROID PowerMate b0f4."+powermateService]
	want := Discovered{Name: "ODROID PowerMate b0f4", Hostname: "odroid-pm-b0f4.local", Address: "192.168.30.4",
		Port: 80, URL: "http://192.168.30.4", Firmware: "v1.4.0-test"}
	if got == nil || *got != want {
		t.Fatalf("got %+v, want %+v", got, want)
	}
	// Without an A record the reply's source address is used.
	if d := found["ODROID PowerMate 1a2b."+powermateService]; d == nil || d.Address != "192.168.30.9" {
		t.Fatalf("got %+v, want address from the packet source", d)
	}
}

// POWERMATE_DISCOVER_LIVE=1 go test -run Live -v  lists the PowerMates on this network.
func TestDiscoverLive(t *testing.T) {
	if os.Getenv("POWERMATE_DISCOVER_LIVE") == "" {
		t.Skip("set POWERMATE_DISCOVER_LIVE=1 to query the local network")
	}
	list, err := Discover(context.Background(), 2*time.Second)
	if err != nil {
		t.Fatal(err)
	}
	for _, d := range list {
		t.Logf("%+v", d)
	}
	if len(list) == 0 {
		t.Fatal("no PowerMate answered")
	}
}

func TestParseAnswerHTTPS(t *testing.T) {
	found := map[string]*Discovered{}
	parseAnswer(replyWith(t, "b0f4", true, 443, []string{"fw=v1.4.1", "tls=1"}), nil, found)
	d := found["ODROID PowerMate b0f4."+powermateService]
	if d == nil || !d.TLS || d.Port != 443 || d.URL != "https://192.168.30.4" {
		t.Fatalf("got %+v, want TLS on 443 at https://192.168.30.4", d)
	}
}
