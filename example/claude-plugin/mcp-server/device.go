// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

package main

import (
	"context"
	"fmt"
	"log"
	"math"
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
	UptimeMS uint64    `json:"device_uptime_ms"`
	Message  string    `json:"message"`
}

// device keeps the PowerMate streams open for the life of the MCP server, so console
// output and power samples are captured between tool calls.
type device struct {
	client  *powermate.Client
	console *console.Buffer

	mu           sync.Mutex
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

func newDevice(client *powermate.Client) *device {
	return &device{client: client, console: console.NewBuffer(consoleBufferBytes)}
}

func (d *device) run(ctx context.Context) {
	go d.client.RunUART(ctx, d.console.Write, func(up bool, err error) {
		d.mu.Lock()
		defer d.mu.Unlock()
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
		if over := len(d.events) - maxEvents; over > 0 {
			d.events = append(d.events[:0], d.events[over:]...)
		}
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
			st.CurrentMax = math.Max(st.CurrentMax, a)
			st.PowerMax = math.Max(st.PowerMax, w)
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

func (m *manager) newClient(host string) (*powermate.Client, error) {
	return powermate.NewClient(host, m.username, m.password,
		fileTokenStore{path: filepath.Join(m.stateDir, "token-"+sanitize(host))})
}

// start connects to host without probing it first (used at startup, when the
// PowerMate may simply not be up yet).
func (m *manager) start(host, source string) error {
	client, err := m.newClient(host)
	if err != nil {
		return err
	}
	m.swap(client, host, source)
	return nil
}

// connect probes host and, if a PowerMate answers, makes it the current device.
func (m *manager) connect(ctx context.Context, host string, save bool) (string, error) {
	// Reconnecting to the current address would throw away the console buffer and
	// power history for nothing, so keep the device when it still answers.
	if host == "" {
		return "", fmt.Errorf("host is empty: ask the user for the PowerMate address")
	}
	if cur, _ := m.hostInfo(); cur == host {
		probe, cancel := context.WithTimeout(ctx, 10*time.Second)
		defer cancel()
		if v, err := m.current().client.GetVersion(probe); err == nil {
			m.saveHost(host, save)
			return v.Version, nil
		}
	}
	client, err := m.newClient(host)
	if err != nil {
		return "", err
	}
	probe, cancel := context.WithTimeout(ctx, 10*time.Second)
	defer cancel()
	version, err := client.GetVersion(probe)
	if err != nil {
		return "", fmt.Errorf("no PowerMate API at %s: %w", host, err)
	}
	m.swap(client, host, "tool")
	m.saveHost(host, save)
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
	dev := newDevice(client)
	dev.run(ctx)

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
