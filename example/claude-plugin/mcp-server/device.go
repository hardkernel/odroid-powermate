// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

package main

import (
	"context"
	"fmt"
	"log"
	"math"
	"net"
	"os"
	"path/filepath"
	"strings"
	"sync"
	"time"

	"github.com/hardkernel/odroid-powermate/example/claude-plugin/mcp-server/internal/console"
	"github.com/hardkernel/odroid-powermate/example/claude-plugin/mcp-server/internal/powermate"
)

const (
	consoleBufferBytes = 4 << 20
	maxSamples         = 20000 // ~33 minutes at the firmware's fastest 100 ms period
	maxEvents          = 500
)

type sample struct {
	At     time.Time
	Sensor powermate.SensorData
}

type event struct {
	At       time.Time `json:"host_time"`
	Level    string    `json:"level"`
	UptimeMS uint64    `json:"device_uptime_ms,omitempty"`
	Message  string    `json:"message"`
}

// device keeps the PowerMate streams open while tools are in use, so console output
// and power samples are captured between the tool calls of a task. After idle without
// a tool call it closes them: the firmware has only seven sockets for every client
// (web UI, pmtui, other Claude sessions), and each open session here holds two.
type device struct {
	client  *powermate.Client
	console *console.Buffer
	ctx     context.Context // lifetime of this device
	idle    time.Duration

	mu           sync.Mutex
	streamCancel context.CancelFunc // nil while the streams are closed
	streamGen    int                // ignores callbacks from streams already closed
	inUse        int                // tool calls running
	lastUse      time.Time
	pausedAt     time.Time
	samples      []sample
	events       []event
	switches     powermate.SwitchStatus
	haveSwitches bool
	statusUp     bool
	uartUp       bool
	statusErr    string
	uartErr      string
	uartUpSince  time.Time
	readCursor   int64 // where sbc_console_read/wait continue by default
}

func newDevice(ctx context.Context, client *powermate.Client, idle time.Duration) *device {
	d := &device{client: client, console: console.NewBuffer(consoleBufferBytes), ctx: ctx, idle: idle}
	go d.closeWhenIdle()
	return d
}

// acquire opens the streams if they are closed and keeps them open until the matching
// release. A fresh start waits briefly for the streams, so the first tool call already
// sees the console and a power sample.
func (d *device) acquire(ctx context.Context) {
	d.mu.Lock()
	d.inUse++
	d.lastUse = time.Now()
	started := d.streamCancel == nil
	if started {
		if !d.pausedAt.IsZero() {
			d.addEventLocked("info", fmt.Sprintf("PowerMate streams reopened after an idle pause: console output "+
				"and power samples from %s to now were not captured", d.pausedAt.Format(time.TimeOnly)))
		}
		streamCtx, cancel := context.WithCancel(d.ctx)
		d.streamCancel = cancel
		d.streamGen++
		d.run(streamCtx, d.streamGen)
	}
	d.mu.Unlock()
	if !started {
		return
	}
	since := time.Now()
	deadline := since.Add(3 * time.Second)
	for time.Now().Before(deadline) && ctx.Err() == nil {
		d.mu.Lock()
		ready := d.uartUp && d.statusUp && len(d.samples) > 0 && d.samples[len(d.samples)-1].At.After(since)
		d.mu.Unlock()
		if ready {
			return
		}
		time.Sleep(50 * time.Millisecond)
	}
}

func (d *device) release() {
	d.mu.Lock()
	d.inUse--
	d.lastUse = time.Now()
	d.mu.Unlock()
}

func (d *device) closeWhenIdle() {
	if d.idle <= 0 {
		return
	}
	ticker := time.NewTicker(min(d.idle/4, 15*time.Second))
	defer ticker.Stop()
	for {
		select {
		case <-d.ctx.Done():
			return
		case <-ticker.C:
		}
		d.mu.Lock()
		if d.streamCancel == nil || d.inUse > 0 || time.Since(d.lastUse) < d.idle {
			d.mu.Unlock()
			continue
		}
		d.streamCancel()
		d.streamCancel = nil
		d.pausedAt = time.Now()
		reason := fmt.Sprintf("paused: no tool call for %s; the next tool call reopens it", d.idle)
		d.uartUp, d.statusUp, d.uartErr, d.statusErr = false, false, reason, reason
		d.mu.Unlock()
		d.client.CloseIdleConnections()
		log.Printf("streams closed after %s idle", d.idle)
	}
}

// run starts both streams; d.mu must be held.
func (d *device) run(ctx context.Context, gen int) {
	go d.client.RunUART(ctx, d.console.Write, func(up bool, err error) {
		d.mu.Lock()
		defer d.mu.Unlock()
		if gen != d.streamGen || ctx.Err() != nil {
			return
		}
		if up && !d.uartUp {
			d.uartUpSince = time.Now()
		}
		d.uartUp = up
		d.uartErr = errString(err)
		if err != nil {
			log.Printf("uart stream: %v", err)
		}
	})
	go d.client.RunStatus(ctx, d.onStatus, func(up bool, err error) {
		d.mu.Lock()
		defer d.mu.Unlock()
		if gen != d.streamGen || ctx.Err() != nil {
			return
		}
		d.statusUp = up
		d.statusErr = errString(err)
		if err != nil {
			log.Printf("status stream: %v", err)
		}
	})
}

func (d *device) onStatus(message powermate.StatusMessage) {
	now := time.Now()
	d.mu.Lock()
	defer d.mu.Unlock()
	switch message.Kind {
	case powermate.PayloadSensor:
		d.samples = append(d.samples, sample{At: now, Sensor: message.Sensor})
		if over := len(d.samples) - maxSamples; over > 0 {
			d.samples = append(d.samples[:0], d.samples[over:]...)
		}
	case powermate.PayloadSwitch:
		d.switches = message.Switch
		d.haveSwitches = true
	case powermate.PayloadEvent:
		d.events = append(d.events, event{
			At:       now,
			Level:    eventLevel(message.Event.Level),
			UptimeMS: message.Event.UptimeMS,
			Message:  message.Event.Message,
		})
		d.trimEventsLocked()
	}
}

// addEventLocked records an event of this server's own; d.mu must be held.
func (d *device) addEventLocked(level, message string) {
	d.events = append(d.events, event{At: time.Now(), Level: level, Message: message})
	d.trimEventsLocked()
}

func (d *device) trimEventsLocked() {
	if over := len(d.events) - maxEvents; over > 0 {
		d.events = append(d.events[:0], d.events[over:]...)
	}
}

// waitUART blocks until the UART stream is connected or timeout passes.
func (d *device) waitUART(ctx context.Context, timeout time.Duration) error {
	deadline := time.Now().Add(timeout)
	for {
		d.mu.Lock()
		up, lastErr := d.uartUp, d.uartErr
		d.mu.Unlock()
		if up {
			return nil
		}
		if time.Now().After(deadline) {
			if lastErr != "" {
				return fmt.Errorf("UART stream to %s is not connected: %s", d.client.Host(), lastErr)
			}
			return fmt.Errorf("UART stream to %s is not connected", d.client.Host())
		}
		select {
		case <-ctx.Done():
			return ctx.Err()
		case <-time.After(100 * time.Millisecond):
		}
	}
}

func (d *device) send(ctx context.Context, data []byte) error {
	if err := d.waitUART(ctx, 5*time.Second); err != nil {
		return err
	}
	return d.client.SendUART(data)
}

func (d *device) setReadCursor(cursor int64) {
	d.mu.Lock()
	if cursor > d.readCursor {
		d.readCursor = cursor
	}
	d.mu.Unlock()
}

func (d *device) getReadCursor() int64 {
	d.mu.Lock()
	defer d.mu.Unlock()
	return d.readCursor
}

func (d *device) samplesSince(from time.Time) []sample {
	d.mu.Lock()
	defer d.mu.Unlock()
	var out []sample
	for i := len(d.samples) - 1; i >= 0 && !d.samples[i].At.Before(from); i-- {
		out = append(out, d.samples[i])
	}
	for i, j := 0, len(out)-1; i < j; i, j = i+1, j-1 {
		out[i], out[j] = out[j], out[i]
	}
	return out
}

func (d *device) latest() (sample, bool) {
	d.mu.Lock()
	defer d.mu.Unlock()
	if len(d.samples) == 0 {
		return sample{}, false
	}
	return d.samples[len(d.samples)-1], true
}

func (d *device) eventsSince(from time.Time) []event {
	d.mu.Lock()
	defer d.mu.Unlock()
	var out []event
	for _, e := range d.events {
		if !e.At.Before(from) {
			out = append(out, e)
		}
	}
	return out
}

func eventLevel(level int32) string {
	switch level {
	case 0:
		return "info"
	case 1:
		return "warning"
	case 2:
		return "critical"
	case 3:
		return "fatal"
	}
	return fmt.Sprintf("level-%d", level)
}

func errString(err error) string {
	if err == nil {
		return ""
	}
	return err.Error()
}

// channelStats summarises one INA3221 channel over a window.
type channelStats struct {
	VoltageAvg float64 `json:"voltage_avg_v"`
	VoltageMin float64 `json:"voltage_min_v"`
	VoltageMax float64 `json:"voltage_max_v"`
	VoltageStd float64 `json:"voltage_stddev_v"`
	CurrentAvg float64 `json:"current_avg_a"`
	CurrentMax float64 `json:"current_max_a"`
	PowerAvg   float64 `json:"power_avg_w"`
	PowerMax   float64 `json:"power_max_w"`
	EnergyWh   float64 `json:"energy_wh"`
}

type powerSummary struct {
	Samples int          `json:"samples"`
	Seconds float64      `json:"seconds"`
	VIN     channelStats `json:"vin"`
	Main    channelStats `json:"main"`
	USB     channelStats `json:"usb"`
	// Warnings flags readings that are physically implausible for this board.
	Warnings []string `json:"warnings,omitempty"`
}

func summarize(samples []sample) powerSummary {
	summary := powerSummary{Samples: len(samples)}
	if len(samples) == 0 {
		return summary
	}
	summary.Seconds = samples[len(samples)-1].At.Sub(samples[0].At).Seconds()
	pick := []func(powermate.SensorData) powermate.ChannelData{
		func(s powermate.SensorData) powermate.ChannelData { return s.VIN },
		func(s powermate.SensorData) powermate.ChannelData { return s.Main },
		func(s powermate.SensorData) powermate.ChannelData { return s.USB },
	}
	out := []*channelStats{&summary.VIN, &summary.Main, &summary.USB}
	for c, get := range pick {
		st := out[c]
		st.VoltageMin = math.Inf(1)
		st.VoltageMax = math.Inf(-1)
		var sumV, sumV2, sumI, sumP float64
		for i, s := range samples {
			ch := get(s.Sensor)
			v, a, w := float64(ch.Voltage), float64(ch.Current), float64(ch.Power)
			sumV += v
			sumV2 += v * v
			sumI += a
			sumP += w
			st.VoltageMin = math.Min(st.VoltageMin, v)
			st.VoltageMax = math.Max(st.VoltageMax, v)
			st.CurrentMax = math.Max(st.CurrentMax, float64(ch.PeakCurrent()))
			st.PowerMax = math.Max(st.PowerMax, float64(ch.PeakPower()))
			if i > 0 {
				st.EnergyWh += w * s.At.Sub(samples[i-1].At).Hours()
			}
		}
		n := float64(len(samples))
		st.VoltageAvg = sumV / n
		st.VoltageStd = math.Sqrt(math.Max(sumV2/n-st.VoltageAvg*st.VoltageAvg, 0))
		st.CurrentAvg = sumI / n
		st.PowerAvg = sumP / n
		roundStats(st)
	}
	summary.Warnings = plausibility(samples, summary)
	return summary
}

// plausibility flags readings that point at a wiring problem rather than the target.
// Without a shared GND between the PowerMate output and the SBC, or with a loose
// connector, the INA3221 reports values that jump around or fall outside its range.
func plausibility(samples []sample, summary powerSummary) []string {
	var warnings []string
	for _, s := range samples {
		for name, ch := range map[string]powermate.ChannelData{"vin": s.Sensor.VIN, "main": s.Sensor.Main, "usb": s.Sensor.USB} {
			if ch.Voltage < -0.5 || ch.Voltage > 26.5 || math.Abs(float64(ch.Current)) > 16 {
				warnings = append(warnings, fmt.Sprintf(
					"%s reading out of sensor range (%.2f V, %.2f A): check the power wiring and GND connection",
					name, ch.Voltage, ch.Current))
				return warnings
			}
		}
	}
	if summary.Samples >= 10 && summary.VIN.VoltageStd > 0.5 {
		warnings = append(warnings, fmt.Sprintf(
			"VIN voltage is unstable (stddev %.2f V): a loose input connector or missing GND gives unreliable readings",
			summary.VIN.VoltageStd))
	}
	return warnings
}

func roundStats(st *channelStats) {
	for _, f := range []*float64{&st.VoltageAvg, &st.VoltageMin, &st.VoltageMax, &st.VoltageStd,
		&st.CurrentAvg, &st.CurrentMax, &st.PowerAvg, &st.PowerMax} {
		*f = math.Round(*f*1000) / 1000
	}
	st.EnergyWh = math.Round(st.EnergyWh*1e6) / 1e6
}

// manager owns the device connection and can retarget it to another PowerMate address
// at runtime, e.g. after onboarding moved it to a new network.
type manager struct {
	root     context.Context
	stateDir string
	envHost  string // POWERMATE_HOST
	envPin   string // POWERMATE_TLS_FINGERPRINT, for envHost

	idle time.Duration // close the streams after this long without a tool call

	mu       sync.Mutex
	dev      *device
	cancel   context.CancelFunc
	host     string
	source   string // where host came from: env, saved, tool
	username string
	password string
}

func (m *manager) current() *device {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.dev
}

func (m *manager) hostInfo() (string, string) {
	m.mu.Lock()
	defer m.mu.Unlock()
	return m.host, m.source
}

// hostKey is the device part of an address ("https://10.0.0.5:443" -> "10.0.0.5"), so
// the token and the certificate pin follow the device across http:// and https://.
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

func (m *manager) newClient(host, pin string) (*powermate.Client, error) {
	if pin == "" {
		pin = m.savedPin(host)
	}
	return powermate.NewClient(host, m.username, m.password,
		fileTokenStore{path: filepath.Join(m.stateDir, "token-"+sanitize(hostKey(host)))}, pin)
}

func (m *manager) pinPath(host string) string {
	return filepath.Join(m.stateDir, "tls-"+sanitize(hostKey(host)))
}

// savedPin is the certificate fingerprint the user confirmed for host:
// POWERMATE_TLS_FINGERPRINT for the POWERMATE_HOST device, else the one saved by
// powermate_connect.
func (m *manager) savedPin(host string) string {
	if m.envPin != "" && hostKey(host) == hostKey(m.envHost) {
		return m.envPin
	}
	data, err := os.ReadFile(m.pinPath(host))
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(data))
}

func (m *manager) savePin(host, pin string) {
	err := os.MkdirAll(m.stateDir, 0o700)
	if err == nil {
		err = os.WriteFile(m.pinPath(host), []byte(pin+"\n"), 0o600)
	}
	if err != nil {
		log.Printf("save certificate fingerprint: %v", err)
	}
}

// start connects to host without probing it first (used at startup, when the
// PowerMate may simply not be up yet).
func (m *manager) start(host, source string) error {
	client, err := m.newClient(host, "")
	if err != nil {
		return err
	}
	m.swap(client, host, source)
	return nil
}

// connect probes host and, if a PowerMate answers, makes it the current device. pin is
// a certificate fingerprint the user confirmed; it is saved for the device on success.
func (m *manager) connect(ctx context.Context, host, pin string, save bool) (string, error) {
	// Reconnecting to the current address would throw away the console buffer and
	// power history for nothing, so keep the device when it still answers.
	if host == "" {
		return "", fmt.Errorf("host is empty: ask the user for the PowerMate address")
	}
	// The current host is stored as found (http:// or https://); compare the device part.
	if cur, _ := m.hostInfo(); hostKey(cur) == hostKey(host) && pin == "" {
		probe, cancel := context.WithTimeout(ctx, 10*time.Second)
		defer cancel()
		if v, err := m.current().client.GetVersion(probe); err == nil {
			m.saveHost(host, save)
			return v.Version, nil
		}
	}
	client, err := m.newClient(host, pin)
	if err != nil {
		return "", err
	}
	probe, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	version, err := client.GetVersion(probe)
	if err != nil {
		return "", fmt.Errorf("no PowerMate API at %s: %w", host, err)
	}
	// Keep the address as found (https:// once the device serves HTTPS), so the next
	// session goes straight to it.
	m.swap(client, client.URL(), "tool")
	m.saveHost(client.URL(), save)
	if client.Pin() != "" && pin != "" {
		m.savePin(host, client.Pin())
	}
	return version.Version, nil
}

func (m *manager) saveHost(host string, save bool) {
	if !save {
		return
	}
	err := os.MkdirAll(m.stateDir, 0o700)
	if err == nil {
		err = os.WriteFile(m.savedHostPath(), []byte(host+"\n"), 0o600)
	}
	if err != nil {
		log.Printf("save host: %v", err)
	}
}

func (m *manager) swap(client *powermate.Client, host, source string) {
	ctx, cancel := context.WithCancel(m.root)
	dev := newDevice(ctx, client, m.idle)

	m.mu.Lock()
	old := m.cancel
	m.dev, m.cancel, m.host, m.source = dev, cancel, host, source
	m.mu.Unlock()
	if old != nil {
		old()
	}
}

func (m *manager) setCredentials(username, password string) {
	m.mu.Lock()
	m.username, m.password = username, password
	dev := m.dev
	m.mu.Unlock()
	dev.client.SetCredentials(username, password)
}

func (m *manager) savedHostPath() string {
	return filepath.Join(m.stateDir, "host")
}

func (m *manager) savedHost() string {
	data, err := os.ReadFile(m.savedHostPath())
	if err != nil {
		return ""
	}
	return strings.TrimSpace(string(data))
}
