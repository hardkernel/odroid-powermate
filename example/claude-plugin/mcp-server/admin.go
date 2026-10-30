// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

package main

import (
	"context"
	"errors"
	"fmt"
	"math"
	"net/netip"
	"net/url"
	"sort"
	"strings"
	"time"

	"github.com/modelcontextprotocol/go-sdk/mcp"

	"github.com/hardkernel/odroid-powermate/example/claude-plugin/mcp-server/internal/powermate"
)

// Limits enforced by the firmware (main/service/climit.h).
const (
	warnMaxVIN, warnMaxMain, warnMaxUSB = 10.0, 10.0, 5.9
	critMaxVIN, critMaxMain, critMaxUSB = 15.0, 11.0, 6.0
	critMin                             = 1.0
)

var supportedBaudRates = []string{"9600", "19200", "38400", "57600", "115200", "230400", "460800", "921600", "1500000"}

const (
	previewNote = "Nothing was changed. Show these risks to the user, and call again with confirm=true " +
		"only after they agree."
	recoveryHint = "Recovery if the PowerMate becomes unreachable: the user connects a PC to its USB debug " +
		"port and runs wifi_status / wifi_connect <ssid> <password> in its console (idf.py monitor or minicom; " +
		"opening the port may reboot the PowerMate and switch the SBC off), or factory-resets it by holding its " +
		"button for 5 s, which erases ALL PowerMate settings and brings back the setup AP for phone setup."
	// lostHint is for a PowerMate whose address is unknown.
	lostHint = "Find its address in the router's DHCP list, or ask the user to run wifi_status in its USB debug " +
		"console, then call powermate_connect. A factory-fresh PowerMate is first set up from a phone through " +
		"its setup AP (powermate / 192.168.4.1)."
)

// adminOut is shared by every powermate_* tool that changes something. Without
// confirm it is a preview: what would be sent, the current state, and the risks.
type adminOut struct {
	Applied  bool           `json:"applied"`
	Request  map[string]any `json:"request,omitempty"`
	Current  map[string]any `json:"current,omitempty"`
	Risks    []string       `json:"risks"`
	Result   map[string]any `json:"result,omitempty"`
	Notes    []string       `json:"notes,omitempty"`
	Warnings []string       `json:"warnings,omitempty"`
}

func registerAdminTools(server *mcp.Server, t *tools) {
	readOnly := &mcp.ToolAnnotations{ReadOnlyHint: true, OpenWorldHint: boolPtr(false)}
	risky := &mcp.ToolAnnotations{DestructiveHint: boolPtr(true), OpenWorldHint: boolPtr(false)}
	note := " Without confirm=true it only previews the change and its risks."

	mcp.AddTool(server, &mcp.Tool{
		Name: "powermate_settings",
		Description: "Current PowerMate configuration: address in use, firmware, Wi-Fi mode and connection, " +
			"IP settings, UART baud rate, sensor period, output-state restore, current limits, diagnostics.",
		Annotations: readOnly,
	}, t.settings)

	mcp.AddTool(server, &mcp.Tool{
		Name: "powermate_connect",
		Description: "Point the tools at a PowerMate address (first use, after onboarding, or to switch boards). " +
			"Use only an address the user gave or a tool reported; never guess one. Checks " +
			"that a PowerMate answers there, then reconnects all streams. save=true (default) remembers it " +
			"for the next session when POWERMATE_HOST is not set.",
		Annotations: &mcp.ToolAnnotations{OpenWorldHint: boolPtr(false)},
	}, t.connect)

	mcp.AddTool(server, &mcp.Tool{
		Name: "powermate_wifi_scan",
		Description: "List Wi-Fi networks the PowerMate can see. The scan blocks the PowerMate for a few " +
			"seconds; the status and UART streams may drop and reconnect, and a STA connection that is " +
			"still being established is interrupted.",
		Annotations: readOnly,
	}, t.wifiScan)

	mcp.AddTool(server, &mcp.Tool{
		Name: "powermate_wifi_connect",
		Description: "Join the PowerMate to a Wi-Fi network (STA). It leaves its current network at once and " +
			"usually gets a new IP address." + note,
		Annotations: risky,
	}, t.wifiConnect)

	mcp.AddTool(server, &mcp.Tool{
		Name:        "powermate_network",
		Description: "Set the PowerMate's STA addressing: dhcp, or static ip/gateway/subnet/dns." + note,
		Annotations: risky,
	}, t.network)

	mcp.AddTool(server, &mcp.Tool{
		Name: "powermate_wifi_mode",
		Description: "Switch the PowerMate between sta (client only) and apsta (client plus its own access " +
			"point at 192.168.4.1), and set the AP name and password." + note,
		Annotations: risky,
	}, t.wifiMode)

	mcp.AddTool(server, &mcp.Tool{
		Name: "powermate_current_limits",
		Description: "Set per-channel current limits in amperes. Exceeding a warning limit makes the " +
			"PowerMate switch that output off (VIN: all outputs); exceeding a critical limit cuts all outputs " +
			"in hardware. Hardware ratings: VOUT 7 A max (above ~4 A continuous the board runs over 100 C), USB " +
			"port 4 A max. The preview compares new limits with the peak current recorded so far." + note,
		Annotations: risky,
	}, t.currentLimits)

	mcp.AddTool(server, &mcp.Tool{
		Name: "powermate_device_options",
		Description: "Set the UART baud rate bridged to the SBC console, the power sensor period, and whether " +
			"outputs are restored after the PowerMate restarts." + note,
		Annotations: risky,
	}, t.deviceOptions)

	mcp.AddTool(server, &mcp.Tool{
		Name:        "powermate_credentials",
		Description: "Change the PowerMate web login used by the web UI, pmtui and this plugin." + note,
		Annotations: risky,
	}, t.credentials)

	mcp.AddTool(server, &mcp.Tool{
		Name:        "powermate_reboot",
		Description: "Reboot the PowerMate itself and wait for it to come back." + note,
		Annotations: risky,
	}, t.reboot)
}

// ---- powermate_settings ----

type settingsOut struct {
	Host        string         `json:"host"`
	HostSource  string         `json:"host_source"`
	Firmware    string         `json:"firmware_version,omitempty"`
	Settings    map[string]any `json:"settings,omitempty"`
	Diagnostics map[string]any `json:"diagnostics,omitempty"`
	Warnings    []string       `json:"warnings,omitempty"`
}

func (t *tools) settings(ctx context.Context, _ *mcp.CallToolRequest, _ statusIn) (*mcp.CallToolResult, settingsOut, error) {
	out := settingsOut{}
	out.Host, out.HostSource = t.m.hostInfo()
	client := t.d().client
	v, err := client.GetVersion(ctx)
	if err != nil {
		return nil, out, fmt.Errorf("PowerMate API at %s unreachable: %w. %s", out.Host, err, lostHint)
	}
	out.Firmware = v.Version
	if out.Settings, err = client.RawSettings(ctx); err != nil {
		out.Warnings = append(out.Warnings, "settings: "+err.Error())
	}
	if out.Diagnostics, err = client.Diagnostics(ctx); err != nil {
		out.Warnings = append(out.Warnings, "diagnostics: "+err.Error())
	}
	if out.Settings["mode"] == "apsta" {
		out.Warnings = append(out.Warnings, "The setup AP is still on (mode apsta): anyone near the PowerMate can "+
			"reach its login page. It is meant for initial phone setup; consider powermate_wifi_mode(sta) once "+
			"the STA network is known to work.")
	}
	return nil, out, nil
}

// ---- powermate_connect ----

type connectIn struct {
	Host string `json:"host" jsonschema:"PowerMate IP address or host name"`
	Save *bool  `json:"save,omitempty" jsonschema:"remember for the next session (default true)"`
}

type connectOut struct {
	Host     string `json:"host"`
	Previous string `json:"previous_host"`
	Firmware string `json:"firmware_version"`
	Note     string `json:"note,omitempty"`
}

func (t *tools) connect(ctx context.Context, _ *mcp.CallToolRequest, in connectIn) (*mcp.CallToolResult, connectOut, error) {
	prev, source := t.m.hostInfo()
	save := in.Save == nil || *in.Save
	version, err := t.m.connect(ctx, strings.TrimSpace(in.Host), save)
	if err != nil {
		return nil, connectOut{Previous: prev}, err
	}
	out := connectOut{Host: in.Host, Previous: prev, Firmware: version}
	if source == "env" {
		out.Note = "POWERMATE_HOST is set in the environment and will win again next session; " +
			"update or unset it to keep this address."
	}
	return nil, out, nil
}

// ---- powermate_wifi_scan ----

type scanOut struct {
	Networks []map[string]any `json:"networks"`
}

func (t *tools) wifiScan(ctx context.Context, _ *mcp.CallToolRequest, _ statusIn) (*mcp.CallToolResult, scanOut, error) {
	aps, err := t.d().client.ScanWiFi(ctx)
	if err != nil {
		return nil, scanOut{}, err
	}
	sort.Slice(aps, func(i, j int) bool { return aps[i].RSSI > aps[j].RSSI })
	out := scanOut{Networks: []map[string]any{}}
	seen := map[string]bool{}
	for _, ap := range aps {
		if ap.SSID == "" || seen[ap.SSID] {
			continue
		}
		seen[ap.SSID] = true
		out.Networks = append(out.Networks, map[string]any{"ssid": ap.SSID, "rssi": ap.RSSI, "auth": ap.AuthMode})
	}
	return nil, out, nil
}

// ---- powermate_wifi_connect ----

type wifiConnectIn struct {
	SSID        string `json:"ssid"`
	Password    string `json:"password,omitempty" jsonschema:"empty for an open network"`
	WaitSeconds int    `json:"wait_seconds,omitempty" jsonschema:"how long to wait for an IP address (default 30, max 90)"`
	Confirm     bool   `json:"confirm,omitempty"`
}

func validateWiFi(ssid, password string) error {
	if ssid == "" || len(ssid) > 32 {
		return errors.New("ssid must be 1 to 32 bytes")
	}
	if password != "" && (len(password) < 8 || len(password) > 63) {
		return errors.New("a WPA password must be 8 to 63 characters (empty for an open network)")
	}
	return nil
}

func (t *tools) wifiConnect(ctx context.Context, _ *mcp.CallToolRequest, in wifiConnectIn) (*mcp.CallToolResult, adminOut, error) {
	if err := validateWiFi(in.SSID, in.Password); err != nil {
		return nil, adminOut{}, err
	}
	client := t.d().client
	host, _ := t.m.hostInfo()
	cur, err := client.RawSettings(ctx)
	if err != nil {
		return nil, adminOut{}, fmt.Errorf("PowerMate unreachable at %s (%w). %s", host, err, lostHint)
	}
	out := adminOut{
		Request: map[string]any{"ssid": in.SSID, "password": mask(in.Password)},
		Current: pick(cur, "mode", "ssid", "connected", "wifi_connection_status", "ip"),
	}
	out.Risks = append(out.Risks,
		fmt.Sprintf("The PowerMate drops its current Wi-Fi (%v) immediately. The tools reach it at %s, so they "+
			"lose it if that address belongs to the current network.", orDash(cur["ssid"]), host),
		"The new network will usually assign a different IP. The tools try to find it; otherwise look it up "+
			"in the router's DHCP list (or wifi_status on the USB debug console) and call powermate_connect.")
	if cur["mode"] == "sta" {
		out.Risks = append(out.Risks, "Mode is sta (no setup AP): if the new network fails, the PowerMate cannot be "+
			"reached over the network at all. "+recoveryHint)
	} else {
		out.Risks = append(out.Risks, "Mode is apsta: if the new network fails, the setup AP stays up, so a phone "+
			"on the powermate AP can still reach 192.168.4.1.")
	}
	if !in.Confirm {
		out.Notes = []string{previewNote}
		return nil, out, nil
	}

	result, dropped, err := postDroppable(ctx, client, map[string]any{"ssid": in.SSID, "password": in.Password})
	out.Applied = err == nil
	out.Result = result
	if err != nil {
		return nil, out, err
	}
	if dropped {
		out.Notes = append(out.Notes, droppedNote)
	}
	// Poll over whichever path still works (the AP path keeps working in apsta mode).
	wait := time.Duration(clampInt(in.WaitSeconds, 30, 90)) * time.Second
	deadline := time.Now().Add(wait)
	unreachable := false
	for time.Now().Before(deadline) {
		select {
		case <-time.After(time.Second):
		case <-ctx.Done():
			return nil, out, ctx.Err()
		}
		st, err := client.RawSettings(ctx)
		if err != nil {
			// Expected while the STA link reconnects; give up only at the deadline.
			unreachable = true
			continue
		}
		unreachable = false
		if st["ssid"] != nil && st["ssid"] != in.SSID && st["wifi_connection_status"] == "connected" {
			continue // still reporting the previous network
		}
		switch st["wifi_connection_status"] {
		case "connected":
			out.Result = pick(st, "ssid", "rssi", "ip", "wifi_connection_status")
			if ip := ipOf(st); ip != "" && ip != host && host != "192.168.4.1" {
				out.Notes = append(out.Notes, "New STA address "+ip+": call powermate_connect(host="+ip+").")
			}
			return nil, out, nil
		case "failed":
			out.Warnings = append(out.Warnings, fmt.Sprintf("connection failed: %v (wrong password or unsupported security)",
				orDash(st["wifi_failure_reason"])))
			return nil, out, nil
		}
	}
	if unreachable {
		out.Notes = append(out.Notes, "PowerMate stopped answering at "+host+" (expected if that address was on "+
			"the old network). "+lostHint)
		return nil, out, nil
	}
	out.Warnings = append(out.Warnings, "no IP address yet; the PowerMate keeps retrying in the background")
	return nil, out, nil
}

// ---- powermate_network ----

type networkIn struct {
	Mode       string `json:"mode" jsonschema:"dhcp or static"`
	IP         string `json:"ip,omitempty"`
	Gateway    string `json:"gateway,omitempty"`
	Subnet     string `json:"subnet,omitempty" jsonschema:"netmask, e.g. 255.255.255.0"`
	DNS1       string `json:"dns1,omitempty"`
	DNS2       string `json:"dns2,omitempty"`
	SwitchHost *bool  `json:"switch_host,omitempty" jsonschema:"for static: point the tools at the new IP afterwards (default true)"`
	Confirm    bool   `json:"confirm,omitempty"`
}

func (t *tools) network(ctx context.Context, _ *mcp.CallToolRequest, in networkIn) (*mcp.CallToolResult, adminOut, error) {
	client := t.d().client
	host, _ := t.m.hostInfo()
	cur, err := client.RawSettings(ctx)
	if err != nil {
		return nil, adminOut{}, err
	}
	out := adminOut{Current: pick(cur, "mode", "net_type", "ssid", "ip")}
	payload := map[string]any{"net_type": in.Mode}
	switch in.Mode {
	case "dhcp":
		out.Risks = append(out.Risks, "The DHCP server may hand out a different address than the current "+orDash(ipOf(cur))+
			"; the tools then lose the PowerMate until powermate_connect is given the new one.")
	case "static":
		if err := validateStatic(in); err != nil {
			return nil, adminOut{}, err
		}
		payload["ip"], payload["gateway"], payload["subnet"], payload["dns1"] = in.IP, in.Gateway, in.Subnet, in.DNS1
		if in.DNS2 != "" {
			payload["dns2"] = in.DNS2
		}
		out.Risks = append(out.Risks,
			"A static address that is wrong for this network, or already used by another device, makes the "+
				"PowerMate unreachable over Wi-Fi. The setting is stored, so it survives a reboot. "+recoveryHint,
			"Make sure "+in.IP+" is outside the router's DHCP pool or reserved for the PowerMate.")
	default:
		return nil, adminOut{}, errors.New("mode must be dhcp or static")
	}
	if cur["mode"] == "sta" {
		out.Risks = append(out.Risks, "Mode is sta, so there is no AP fallback.")
	}
	out.Request = payload
	if !in.Confirm {
		out.Notes = []string{previewNote}
		return nil, out, nil
	}
	result, dropped, err := postDroppable(ctx, client, payload)
	out.Applied, out.Result = err == nil, result
	if err != nil {
		return nil, out, err
	}
	if dropped {
		out.Notes = append(out.Notes, droppedNote)
	}
	if in.Mode == "static" && (in.SwitchHost == nil || *in.SwitchHost) && host == ipOf(cur) && in.IP != host {
		time.Sleep(2 * time.Second)
		if v, err := t.m.connect(ctx, in.IP, true); err == nil {
			out.Notes = append(out.Notes, "Tools now use "+in.IP+" (firmware "+v+").")
		} else {
			out.Warnings = append(out.Warnings, "could not reach the new address yet: "+err.Error())
		}
	}
	return nil, out, nil
}

func validateStatic(in networkIn) error {
	parse := func(name, v string) (netip.Addr, error) {
		a, err := netip.ParseAddr(v)
		if err != nil || !a.Is4() {
			return a, fmt.Errorf("%s %q is not an IPv4 address", name, v)
		}
		return a, nil
	}
	ip, err := parse("ip", in.IP)
	if err != nil {
		return err
	}
	gw, err := parse("gateway", in.Gateway)
	if err != nil {
		return err
	}
	mask, err := parse("subnet", in.Subnet)
	if err != nil {
		return err
	}
	if _, err := parse("dns1", in.DNS1); err != nil {
		return err
	}
	if in.DNS2 != "" {
		if _, err := parse("dns2", in.DNS2); err != nil {
			return err
		}
	}
	m := mask.As4()
	bits := 0
	ones := true
	for _, b := range m {
		for i := 7; i >= 0; i-- {
			if b&(1<<i) != 0 {
				if !ones {
					return fmt.Errorf("subnet %s is not a valid netmask", in.Subnet)
				}
				bits++
			} else {
				ones = false
			}
		}
	}
	if bits < 8 || bits > 30 {
		return fmt.Errorf("subnet /%d is not usable", bits)
	}
	prefix := netip.PrefixFrom(ip, bits).Masked()
	if !prefix.Contains(gw) {
		return fmt.Errorf("gateway %s is not in %s", in.Gateway, prefix)
	}
	if ip == prefix.Addr() || ip == broadcast(prefix) {
		return fmt.Errorf("%s is the network or broadcast address of %s", in.IP, prefix)
	}
	if ip == gw {
		return errors.New("ip and gateway are the same address")
	}
	return nil
}

func broadcast(p netip.Prefix) netip.Addr {
	a := p.Addr().As4()
	for i := p.Bits(); i < 32; i++ {
		a[i/8] |= 1 << (7 - i%8)
	}
	return netip.AddrFrom4(a)
}

// ---- powermate_wifi_mode ----

type wifiModeIn struct {
	Mode       string `json:"mode" jsonschema:"sta or apsta"`
	APSSID     string `json:"ap_ssid,omitempty" jsonschema:"for apsta: access point name (default keeps the current one)"`
	APPassword string `json:"ap_password,omitempty" jsonschema:"for apsta: 8 to 63 characters"`
	OpenAP     bool   `json:"open_ap,omitempty" jsonschema:"for apsta: run the AP without a password"`
	Confirm    bool   `json:"confirm,omitempty"`
}

func (t *tools) wifiMode(ctx context.Context, _ *mcp.CallToolRequest, in wifiModeIn) (*mcp.CallToolResult, adminOut, error) {
	client := t.d().client
	host, _ := t.m.hostInfo()
	cur, err := client.RawSettings(ctx)
	if err != nil {
		return nil, adminOut{}, err
	}
	out := adminOut{Current: pick(cur, "mode", "ssid", "connected", "ip")}
	payload := map[string]any{"mode": in.Mode}
	out.Risks = append(out.Risks, "Wi-Fi restarts: every connection to the PowerMate drops for a few seconds.")
	switch in.Mode {
	case "sta":
		if cur["connected"] != true {
			return nil, adminOut{}, errors.New("the STA link is not connected; switching to sta now would leave " +
				"no network path to the PowerMate. Connect it to Wi-Fi first")
		}
		if host == "192.168.4.1" {
			return nil, adminOut{}, errors.New("the tools reach the PowerMate through its AP; switch to its STA " +
				"address with powermate_connect first")
		}
		out.Risks = append(out.Risks, "The setup AP (192.168.4.1) disappears. If the STA network is lost later, "+
			"only the USB debug console or a factory reset gets back in. "+recoveryHint)
	case "apsta":
		if in.APSSID != "" {
			if len(in.APSSID) > 32 {
				return nil, adminOut{}, errors.New("ap_ssid must be at most 32 bytes")
			}
			payload["ap_ssid"] = in.APSSID
		}
		switch {
		case in.APPassword != "":
			if len(in.APPassword) < 8 || len(in.APPassword) > 63 {
				return nil, adminOut{}, errors.New("ap_password must be 8 to 63 characters")
			}
			payload["ap_password"] = in.APPassword
		case in.OpenAP:
			out.Risks = append(out.Risks, "The AP runs WITHOUT a password: anyone nearby can join it and reach "+
				"the PowerMate login page, which can switch the SBC's power and type on its console. Keep a "+
				"strong PowerMate web password if you use this.")
		default:
			// The firmware deletes the AP password when none is sent (open AP), so never
			// send apsta without one unless the user chose an open AP.
			return nil, adminOut{}, errors.New("give ap_password, or set open_ap=true to run an open access point")
		}
		out.Risks = append(out.Risks, "Anyone in radio range who knows the AP password can reach the PowerMate login page.")
	default:
		return nil, adminOut{}, errors.New("mode must be sta or apsta")
	}
	out.Request = maskMap(payload)
	if !in.Confirm {
		out.Notes = []string{previewNote}
		return nil, out, nil
	}
	result, dropped, err := postDroppable(ctx, client, payload)
	out.Applied, out.Result = err == nil, result
	if err != nil || !dropped {
		return nil, out, err
	}
	out.Notes = append(out.Notes, droppedNote)
	deadline := time.Now().Add(30 * time.Second)
	for time.Now().Before(deadline) {
		time.Sleep(2 * time.Second)
		if st, err := client.RawSettings(ctx); err == nil {
			out.Result = pick(st, "mode", "connected", "ssid")
			if st["mode"] == in.Mode {
				return nil, out, nil
			}
		}
	}
	out.Warnings = append(out.Warnings, "could not confirm the new mode within 30 s; check powermate_settings; "+lostHint)
	return nil, out, nil
}

// ---- powermate_current_limits ----

type limitsIn struct {
	VIN          *float64 `json:"vin,omitempty" jsonschema:"VIN warning limit in A, 0 disables (max 10)"`
	Main         *float64 `json:"main,omitempty" jsonschema:"MAIN warning limit in A, 0 disables (max 10)"`
	USB          *float64 `json:"usb,omitempty" jsonschema:"USB warning limit in A, 0 disables (max 5.9)"`
	VINCritical  *float64 `json:"vin_critical,omitempty" jsonschema:"VIN critical limit in A (1 to 15)"`
	MainCritical *float64 `json:"main_critical,omitempty" jsonschema:"MAIN critical limit in A (1 to 11)"`
	USBCritical  *float64 `json:"usb_critical,omitempty" jsonschema:"USB critical limit in A (1 to 6)"`
	Confirm      bool     `json:"confirm,omitempty"`
}

func (t *tools) currentLimits(ctx context.Context, _ *mcp.CallToolRequest, in limitsIn) (*mcp.CallToolResult, adminOut, error) {
	client := t.d().client
	cur, err := client.RawSettings(ctx)
	if err != nil {
		return nil, adminOut{}, err
	}
	out := adminOut{Current: pick(cur, "vin_current_limit", "main_current_limit", "usb_current_limit",
		"vin_critical_current_limit", "main_critical_current_limit", "usb_critical_current_limit")}

	type channel struct {
		name           string
		warn, crit     *float64
		warnKey, crKey string
		warnMax        float64
		critMax        float64
		peak           float64
	}
	samples := t.d().samplesSince(time.Time{})
	peak := func(get func(sample) float32) float64 {
		p := 0.0
		for _, s := range samples {
			p = math.Max(p, float64(get(s)))
		}
		return p
	}
	channels := []channel{
		{"vin", in.VIN, in.VINCritical, "vin_current_limit", "vin_critical_current_limit", warnMaxVIN, critMaxVIN,
			peak(func(s sample) float32 { return s.Sensor.VIN.Current })},
		{"main", in.Main, in.MainCritical, "main_current_limit", "main_critical_current_limit", warnMaxMain, critMaxMain,
			peak(func(s sample) float32 { return s.Sensor.Main.Current })},
		{"usb", in.USB, in.USBCritical, "usb_current_limit", "usb_critical_current_limit", warnMaxUSB, critMaxUSB,
			peak(func(s sample) float32 { return s.Sensor.USB.Current })},
	}
	payload := map[string]any{}
	window := 0.0
	if len(samples) > 1 {
		window = samples[len(samples)-1].At.Sub(samples[0].At).Minutes()
	}
	for _, ch := range channels {
		warn, _ := cur[ch.warnKey].(float64)
		crit, _ := cur[ch.crKey].(float64)
		if ch.warn != nil {
			if *ch.warn < 0 || *ch.warn > ch.warnMax {
				return nil, adminOut{}, fmt.Errorf("%s warning limit must be 0 to %.1f A", ch.name, ch.warnMax)
			}
			warn = *ch.warn
			payload[ch.warnKey] = warn
		}
		if ch.crit != nil {
			if *ch.crit < critMin || *ch.crit > ch.critMax {
				return nil, adminOut{}, fmt.Errorf("%s critical limit must be %.1f to %.1f A", ch.name, critMin, ch.critMax)
			}
			crit = *ch.crit
			payload[ch.crKey] = crit
		}
		if ch.warn == nil && ch.crit == nil {
			continue
		}
		if warn > 0 && warn >= crit {
			return nil, adminOut{}, fmt.Errorf("%s warning limit (%.2f A) must be below its critical limit (%.2f A)", ch.name, warn, crit)
		}
		if ch.peak > 0 {
			if warn > 0 && warn < ch.peak*1.25 {
				out.Risks = append(out.Risks, fmt.Sprintf("%s warning limit %.2f A is within 25%% of the %.2f A peak "+
					"recorded in the last %.0f min: the PowerMate may cut the SBC off during boot or load spikes.",
					ch.name, warn, ch.peak, window))
			}
			if crit < ch.peak*1.5 {
				out.Risks = append(out.Risks, fmt.Sprintf("%s critical limit %.2f A is close to the %.2f A recorded peak.",
					ch.name, crit, ch.peak))
			}
		}
		if warn == 0 {
			out.Risks = append(out.Risks, ch.name+" warning limit disabled: only the critical hardware cutoff protects this channel.")
		}
		if oldWarn, _ := cur[ch.warnKey].(float64); ch.warn != nil && warn > 0 && warn < oldWarn {
			out.Risks = append(out.Risks, fmt.Sprintf("%s warning limit is being lowered from %.2f A to %.2f A. The "+
				"recorded peak (%.2f A) only reflects what ran in the last %.0f min. Unless that included a full "+
				"load test (CPU, GPU, USB devices, displays), real use can exceed the new limit and the board will "+
				"be cut off.", ch.name, oldWarn, warn, ch.peak, window))
		}
	}
	if len(payload) == 0 {
		return nil, adminOut{}, errors.New("give at least one limit")
	}
	if len(samples) == 0 || window < 1 {
		out.Notes = append(out.Notes, "Little power history is available for comparison. Measuring a boot "+
			"(sbc_boot_capture) and a load run first shows the real peaks.")
	}
	for key, rating := range map[string]float64{"main_current_limit": 4, "vin_current_limit": 4, "usb_current_limit": 4} {
		if v, ok := payload[key].(float64); ok && v > rating {
			if key == "usb_current_limit" {
				out.Risks = append(out.Risks, fmt.Sprintf("USB warning limit %.2f A is above the USB port's 4 A rating.", v))
			} else {
				out.Risks = append(out.Risks, fmt.Sprintf("%s %.2f A allows continuous loads above 4 A, where the "+
					"PowerMate's buck converter and load switch run hotter than 100 C (VOUT max is 7 A).",
					strings.TrimSuffix(key, "_current_limit"), v))
			}
		}
	}
	out.Risks = append(out.Risks,
		"Above a warning limit, that output is switched off (VIN: all outputs). Above a critical limit, all "+
			"outputs are cut immediately in hardware.",
		"Higher limits mean less protection for the SBC, cables and power supply in a short circuit.")
	out.Request = payload
	if !in.Confirm {
		out.Notes = append(out.Notes, previewNote)
		return nil, out, nil
	}
	result, err := client.PostSettings(ctx, payload)
	out.Applied, out.Result = err == nil, result
	return nil, out, err
}

// ---- powermate_device_options ----

type optionsIn struct {
	UARTBaudRate       string `json:"uart_baud_rate,omitempty" jsonschema:"9600, 19200, 38400, 57600, 115200, 230400, 460800, 921600 or 1500000"`
	SensorPeriodMS     int    `json:"sensor_period_ms,omitempty" jsonschema:"100 to 10000"`
	RestoreOutputState *bool  `json:"restore_output_state,omitempty" jsonschema:"restore MAIN/USB after the PowerMate restarts"`
	Confirm            bool   `json:"confirm,omitempty"`
}

func (t *tools) deviceOptions(ctx context.Context, _ *mcp.CallToolRequest, in optionsIn) (*mcp.CallToolResult, adminOut, error) {
	client := t.d().client
	cur, err := client.RawSettings(ctx)
	if err != nil {
		return nil, adminOut{}, err
	}
	out := adminOut{Current: pick(cur, "baudrate", "period", "restore_output_state")}
	payload := map[string]any{}
	if in.UARTBaudRate != "" {
		if !contains(supportedBaudRates, in.UARTBaudRate) {
			return nil, adminOut{}, fmt.Errorf("unsupported baud rate %q (supported: %s)", in.UARTBaudRate,
				strings.Join(supportedBaudRates, ", "))
		}
		payload["baudrate"] = in.UARTBaudRate
		out.Risks = append(out.Risks, "The baud rate must match the SBC's console (ODROID-M1/M2 and other "+
			"Rockchip boards: 1500000; most others: 115200). A mismatch turns the console into line noise.")
	}
	if in.SensorPeriodMS != 0 {
		if in.SensorPeriodMS < 100 || in.SensorPeriodMS > 10000 {
			return nil, adminOut{}, errors.New("sensor_period_ms must be 100 to 10000")
		}
		payload["period"] = fmt.Sprint(in.SensorPeriodMS)
		out.Risks = append(out.Risks, "Short periods mean more Wi-Fi traffic and load on the PowerMate. Long "+
			"periods give coarse power data and miss short spikes in sbc_power_record.")
	}
	if in.RestoreOutputState != nil {
		payload["restore_output_state"] = *in.RestoreOutputState
		if *in.RestoreOutputState {
			out.Risks = append(out.Risks, "After any PowerMate restart (power loss, reboot, firmware update), the "+
				"SBC is powered on again without anyone asking.")
		} else {
			out.Risks = append(out.Risks, "After any PowerMate restart, the SBC stays off until switched on.")
		}
	}
	if len(payload) == 0 {
		return nil, adminOut{}, errors.New("give at least one option")
	}
	out.Request = payload
	if !in.Confirm {
		out.Notes = []string{previewNote}
		return nil, out, nil
	}
	result, err := client.PostSettings(ctx, payload)
	out.Applied, out.Result = err == nil, result
	return nil, out, err
}

// ---- powermate_credentials ----

type credentialsIn struct {
	Username string `json:"username"`
	Password string `json:"password"`
	Confirm  bool   `json:"confirm,omitempty"`
}

func (t *tools) credentials(ctx context.Context, _ *mcp.CallToolRequest, in credentialsIn) (*mcp.CallToolResult, adminOut, error) {
	// The firmware reads the login request into a 100-byte buffer, so the JSON
	// {"username":"…","password":"…"} must stay short and free of escapes.
	for _, v := range []string{in.Username, in.Password} {
		if v == "" || strings.ContainsAny(v, "\"\\") || strings.IndexFunc(v, func(r rune) bool { return r < 0x20 || r > 0x7e }) >= 0 {
			return nil, adminOut{}, errors.New("username and password must be non-empty printable ASCII without quotes or backslashes")
		}
	}
	if len(in.Username)+len(in.Password) > 64 {
		return nil, adminOut{}, errors.New("username and password together must be at most 64 characters (firmware login buffer)")
	}
	out := adminOut{
		Request: map[string]any{"new_username": in.Username, "new_password": mask(in.Password)},
		Risks: []string{
			"The web UI and pmtui need the new login.",
			"Set POWERMATE_USERNAME/POWERMATE_PASSWORD for this plugin to the new values. Otherwise the next " +
				"session cannot log in once the cached token stops working (it is lost when the PowerMate reboots).",
			"Tokens issued before the change stay valid until the PowerMate reboots.",
			"Forgetting the password means a factory reset (5 s button hold), which also erases Wi-Fi and limits.",
		},
	}
	if !in.Confirm {
		out.Notes = []string{previewNote}
		return nil, out, nil
	}
	result, err := t.d().client.PostSettings(ctx, map[string]any{"new_username": in.Username, "new_password": in.Password})
	out.Applied, out.Result = err == nil, result
	if err == nil {
		t.m.setCredentials(in.Username, in.Password)
	}
	return nil, out, err
}

// ---- powermate_reboot ----

type confirmIn struct {
	Confirm bool `json:"confirm,omitempty"`
}

func (t *tools) reboot(ctx context.Context, _ *mcp.CallToolRequest, in confirmIn) (*mcp.CallToolResult, adminOut, error) {
	client := t.d().client
	cur, err := client.RawSettings(ctx)
	if err != nil {
		return nil, adminOut{}, err
	}
	control, _ := client.GetControl(ctx)
	out := adminOut{Current: pick(cur, "restore_output_state")}
	out.Current["main_output_on"], out.Current["usb_output_on"] = control.Main, control.USB
	if cur["restore_output_state"] != true && (control.Main || control.USB) {
		out.Risks = append(out.Risks, "Output-state restore is off: the SBC LOSES POWER and stays off after the reboot.")
	}
	out.Risks = append(out.Risks, "Network access, the console stream and power recording stop for about 5 to 15 s.")
	if !in.Confirm {
		out.Notes = []string{previewNote}
		return nil, out, nil
	}
	if err := client.Reboot(ctx); err != nil {
		return nil, out, err
	}
	out.Applied = true
	time.Sleep(5 * time.Second)
	deadline := time.Now().Add(40 * time.Second)
	for time.Now().Before(deadline) {
		probe, cancel := context.WithTimeout(ctx, 3*time.Second)
		v, err := client.GetVersion(probe)
		cancel()
		if err == nil {
			out.Result = map[string]any{"firmware_version": v.Version}
			return nil, out, nil
		}
		time.Sleep(time.Second)
	}
	out.Warnings = append(out.Warnings, "the PowerMate did not answer within 45 s; "+lostHint)
	return nil, out, nil
}

// ---- helpers ----

const droppedNote = "The PowerMate closed the connection while applying the change (it restarts the link " +
	"the request came in on), so its reply was lost; the result below was checked afterwards."

// postDroppable sends a network change. Such a change can cut the very connection
// the request arrived on before the firmware answers, so a transport error counts
// as "sent, verify afterwards" rather than as a failure. Rejections still fail.
func postDroppable(ctx context.Context, client *powermate.Client, payload map[string]any) (map[string]any, bool, error) {
	result, err := client.PostSettings(ctx, payload)
	var transport *url.Error
	if err != nil && errors.As(err, &transport) {
		return result, true, nil
	}
	return result, false, err
}

func mask(secret string) string {
	if secret == "" {
		return ""
	}
	return "********"
}

func maskMap(m map[string]any) map[string]any {
	out := map[string]any{}
	for k, v := range m {
		if strings.Contains(k, "password") {
			v = mask(fmt.Sprint(v))
		}
		out[k] = v
	}
	return out
}

func pick(m map[string]any, keys ...string) map[string]any {
	out := map[string]any{}
	for _, k := range keys {
		if v, ok := m[k]; ok {
			out[k] = v
		}
	}
	return out
}

func ipOf(settings map[string]any) string {
	if ip, ok := settings["ip"].(map[string]any); ok {
		s, _ := ip["ip"].(string)
		return s
	}
	return ""
}

func orDash(v any) string {
	if v == nil || v == "" {
		return "-"
	}
	return fmt.Sprint(v)
}

func contains(list []string, v string) bool {
	for _, x := range list {
		if x == v {
			return true
		}
	}
	return false
}
