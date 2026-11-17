// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

package main

// mDNS discovery of PowerMates (_powermate._tcp.local). The query goes out from an
// ephemeral port, so the PowerMates answer it with a unicast reply straight back to that
// port (RFC 6762 section 6.7). That needs no port 5353 of our own, which avahi or
// systemd-resolved may already hold, and no system mDNS resolver for ".local" names.

import (
	"context"
	"fmt"
	"net"
	"net/netip"
	"sort"
	"strings"
	"time"

	"golang.org/x/net/dns/dnsmessage"
	"golang.org/x/net/ipv4"
)

const powermateService = "_powermate._tcp.local."

var mdnsGroup = &net.UDPAddr{IP: net.IPv4(224, 0, 0, 251), Port: 5353}

// Discovered is one PowerMate that answered a discovery query.
type Discovered struct {
	Name     string `json:"name"`     // instance name, e.g. "ODROID PowerMate b0f4"
	Hostname string `json:"hostname"` // e.g. "odroid-pm-b0f4.local"
	Address  string `json:"address"`  // IPv4 address to connect to
	Port     int    `json:"port"`
	TLS      bool   `json:"tls"` // the web UI and API are served over HTTPS
	URL      string `json:"url"` // base URL to connect to, e.g. https://192.168.30.4
	Firmware string `json:"firmware,omitempty"`
}

// Discover asks the local networks for PowerMates and collects the answers until
// timeout or ctx ends. It returns every PowerMate found, sorted by hostname.
func Discover(ctx context.Context, timeout time.Duration) ([]Discovered, error) {
	conn, err := net.ListenUDP("udp4", &net.UDPAddr{IP: net.IPv4zero})
	if err != nil {
		return nil, err
	}
	defer conn.Close()

	query, err := mdnsQuery()
	if err != nil {
		return nil, err
	}

	ctx, cancel := context.WithTimeout(ctx, timeout)
	defer cancel()
	go func() {
		<-ctx.Done()
		conn.SetReadDeadline(time.Now())
	}()

	// Ask on every multicast-capable interface, and again in case a packet was lost.
	go func() {
		for _, delay := range []time.Duration{0, 250 * time.Millisecond, time.Second} {
			select {
			case <-ctx.Done():
				return
			case <-time.After(delay):
			}
			sendQuery(conn, query)
		}
	}()

	found := map[string]*Discovered{} // by instance name
	buf := make([]byte, 9000)
	var sendErr error
	for {
		n, from, err := conn.ReadFromUDP(buf)
		if err != nil {
			if ctx.Err() != nil {
				break
			}
			sendErr = err
			break
		}
		parseAnswer(buf[:n], from, found)
	}

	list := make([]Discovered, 0, len(found))
	for _, d := range found {
		if d.Address != "" {
			list = append(list, *d)
		}
	}
	sort.Slice(list, func(i, j int) bool { return list[i].Hostname < list[j].Hostname })
	if len(list) == 0 && sendErr != nil {
		return nil, sendErr
	}
	return list, nil
}

func mdnsQuery() ([]byte, error) {
	name, err := dnsmessage.NewName(powermateService)
	if err != nil {
		return nil, err
	}
	msg := dnsmessage.Message{
		Questions: []dnsmessage.Question{{Name: name, Type: dnsmessage.TypePTR, Class: dnsmessage.ClassINET}},
	}
	return msg.Pack()
}

func sendQuery(conn *net.UDPConn, query []byte) {
	pc := ipv4.NewPacketConn(conn)
	ifaces, _ := net.Interfaces()
	sent := false
	for i := range ifaces {
		iface := &ifaces[i]
		if iface.Flags&net.FlagUp == 0 || iface.Flags&net.FlagMulticast == 0 || iface.Flags&net.FlagLoopback != 0 {
			continue
		}
		if pc.SetMulticastInterface(iface) != nil {
			continue
		}
		if _, err := conn.WriteToUDP(query, mdnsGroup); err == nil {
			sent = true
		}
	}
	if !sent {
		conn.WriteToUDP(query, mdnsGroup) // default route
	}
}

// parseAnswer adds what one response says about PowerMate instances to found.
func parseAnswer(packet []byte, from *net.UDPAddr, found map[string]*Discovered) {
	var msg dnsmessage.Message
	if msg.Unpack(packet) != nil || !msg.Header.Response {
		return
	}
	records := append(append(msg.Answers, msg.Authorities...), msg.Additionals...)

	touched := map[*Discovered]bool{}
	entry := func(instance string) *Discovered {
		d := found[instance]
		if d == nil {
			d = &Discovered{Name: instanceLabel(instance)}
			found[instance] = d
		}
		touched[d] = true
		return d
	}
	hosts := map[string]string{} // hostname -> IPv4
	for _, r := range records {
		if a, ok := r.Body.(*dnsmessage.AResource); ok {
			hosts[strings.ToLower(r.Header.Name.String())] = netip.AddrFrom4(a.A).String()
		}
	}
	for _, r := range records {
		switch body := r.Body.(type) {
		case *dnsmessage.PTRResource:
			if strings.EqualFold(r.Header.Name.String(), powermateService) {
				entry(body.PTR.String())
			}
		case *dnsmessage.SRVResource:
			if strings.HasSuffix(strings.ToLower(r.Header.Name.String()), powermateService) {
				d := entry(r.Header.Name.String())
				d.Hostname = strings.TrimSuffix(body.Target.String(), ".")
				d.Port = int(body.Port)
			}
		case *dnsmessage.TXTResource:
			if strings.HasSuffix(strings.ToLower(r.Header.Name.String()), powermateService) {
				d := entry(r.Header.Name.String())
				for _, kv := range body.TXT {
					if v, ok := strings.CutPrefix(kv, "fw="); ok {
						d.Firmware = v
					}
					if kv == "tls=1" {
						d.TLS = true
					}
				}
			}
		}
	}
	for d := range touched {
		if d.Address == "" && d.Hostname != "" {
			d.Address = hosts[strings.ToLower(d.Hostname)+"."]
		}
		// A reply without the A record still comes from the PowerMate itself.
		if d.Address == "" && from != nil && from.IP.To4() != nil && d.Hostname != "" {
			d.Address = from.IP.String()
		}
		if d.Port == 0 {
			d.Port = 80
		}
		d.TLS = d.TLS || d.Port == 443
		d.URL = baseURL(d.Address, d.Port, d.TLS)
	}
}

func baseURL(address string, port int, tls bool) string {
	if address == "" {
		return ""
	}
	scheme, defaultPort := "http", 80
	if tls {
		scheme, defaultPort = "https", 443
	}
	if port == defaultPort {
		return scheme + "://" + address
	}
	return fmt.Sprintf("%s://%s:%d", scheme, address, port)
}

// instanceLabel turns "ODROID\ PowerMate\ b0f4._powermate._tcp.local." into "ODROID PowerMate b0f4".
func instanceLabel(instance string) string {
	label := strings.TrimSuffix(instance, "."+powermateService)
	return strings.ReplaceAll(label, `\ `, " ")
}
