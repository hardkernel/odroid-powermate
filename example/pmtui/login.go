// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

package main

import (
	"context"
	"fmt"
	"strings"
	"time"

	"charm.land/bubbles/v2/textinput"
	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"
)

type loginModel struct {
	inputs     [3]textinput.Model
	focus      int
	submitting bool
	status     string

	// PowerMates found over mDNS; Ctrl+N/Ctrl+P copy one into the Host field.
	found       []Discovered
	selected    int // index into found, -1 when the Host field was typed
	discovering bool
	discoverErr error
}

// discoveredMsg carries the result of an mDNS discovery run.
type discoveredMsg struct {
	found []Discovered
	err   error
}

const discoverWait = 2 * time.Second

func discoverCmd(ctx context.Context) tea.Cmd {
	return func() tea.Msg {
		found, err := Discover(ctx, discoverWait)
		return discoveredMsg{found: found, err: err}
	}
}

func newLoginModel(defaultHost, defaultUsername string) loginModel {
	host := textinput.New()
	host.Prompt = ""
	host.Placeholder = "192.168.4.1"
	host.CharLimit = 255
	host.SetWidth(36)
	host.SetValue(defaultHost)

	username := textinput.New()
	username.Prompt = ""
	username.Placeholder = "admin"
	username.CharLimit = 64
	username.SetWidth(36)
	username.SetValue(defaultUsername)

	password := textinput.New()
	password.Prompt = ""
	password.Placeholder = "Password"
	password.CharLimit = 128
	password.EchoMode = textinput.EchoPassword
	password.EchoCharacter = '*'
	password.SetWidth(36)

	model := loginModel{
		inputs:      [3]textinput.Model{host, username, password},
		selected:    -1,
		discovering: true,
	}
	if strings.TrimSpace(defaultHost) != "" &&
		strings.TrimSpace(defaultUsername) != "" {
		model.focus = 2
	}
	model.focusInput()
	return model
}

func (m *loginModel) Init() tea.Cmd {
	return m.inputs[m.focus].Focus()
}

func (m *loginModel) focusInput() tea.Cmd {
	var cmd tea.Cmd
	for index := range m.inputs {
		if index == m.focus {
			cmd = m.inputs[index].Focus()
		} else {
			m.inputs[index].Blur()
		}
	}
	return cmd
}

// Discovered records an mDNS result. The Host field is left alone: the user picks.
func (m *loginModel) Discovered(msg discoveredMsg) {
	m.discovering = false
	m.found, m.discoverErr = msg.found, msg.err
	m.selected = -1
	for i, d := range m.found {
		if d.Address == m.inputs[0].Value() || d.Hostname == m.inputs[0].Value() {
			m.selected = i
		}
	}
}

// Rediscover marks a new discovery run as started.
func (m *loginModel) Rediscover() {
	m.discovering = true
	m.discoverErr = nil
}

func (m *loginModel) pick(step int) {
	if len(m.found) == 0 {
		return
	}
	if m.selected < 0 {
		if step > 0 {
			m.selected = 0
		} else {
			m.selected = len(m.found) - 1
		}
	} else {
		m.selected = (m.selected + step + len(m.found)) % len(m.found)
	}
	m.inputs[0].SetValue(m.found[m.selected].Address)
	m.inputs[0].CursorEnd()
}

func (m *loginModel) Update(msg tea.Msg) (tea.Cmd, bool) {
	if m.submitting {
		return nil, false
	}

	if key, ok := msg.(tea.KeyPressMsg); ok {
		switch key.String() {
		case "ctrl+n":
			m.pick(1)
			return nil, false
		case "ctrl+p":
			m.pick(-1)
			return nil, false
		case "tab", "down":
			m.focus = (m.focus + 1) % len(m.inputs)
			return m.focusInput(), false
		case "shift+tab", "up":
			m.focus = (m.focus + len(m.inputs) - 1) % len(m.inputs)
			return m.focusInput(), false
		case "enter":
			if m.focus < len(m.inputs)-1 {
				m.focus++
				return m.focusInput(), false
			}
			return nil, true
		}
	}

	var cmd tea.Cmd
	before := m.inputs[0].Value()
	m.inputs[m.focus], cmd = m.inputs[m.focus].Update(msg)
	if m.inputs[0].Value() != before {
		m.selected = -1 // typed over the picked address
	}
	return cmd, false
}

func (m *loginModel) discoveryView() string {
	dim := lipgloss.NewStyle().Foreground(lipgloss.Color("244"))
	switch {
	case m.discovering:
		return dim.Render("Looking for PowerMates on the network...")
	case m.discoverErr != nil:
		return dim.Render("Network discovery failed: " + m.discoverErr.Error())
	case len(m.found) == 0:
		return dim.Render("No PowerMate found on this network (F5 rescan)")
	}
	lines := []string{dim.Render("On this network (Ctrl+N/Ctrl+P pick, F5 rescan):")}
	for i, d := range m.found {
		marker := "  "
		style := lipgloss.NewStyle()
		if i == m.selected {
			marker = "› "
			style = style.Foreground(lipgloss.Color("220"))
		}
		line := fmt.Sprintf("%-15s %s", d.Address, strings.TrimSuffix(d.Hostname, ".local"))
		if d.Firmware != "" {
			line += "  " + d.Firmware
		}
		lines = append(lines, style.Render(marker+line))
	}
	return lipgloss.JoinVertical(lipgloss.Left, lines...)
}

func (m *loginModel) Credentials() (host, username, password string) {
	return strings.TrimSpace(m.inputs[0].Value()),
		strings.TrimSpace(m.inputs[1].Value()),
		m.inputs[2].Value()
}

func (m *loginModel) Begin() {
	m.submitting = true
	m.status = "Connecting..."
}

func (m *loginModel) Failed(err error) tea.Cmd {
	m.submitting = false
	m.status = err.Error()
	m.inputs[2].Reset()
	m.focus = 2
	return m.focusInput()
}

func (m *loginModel) View(width, height int) string {
	label := lipgloss.NewStyle().
		Foreground(lipgloss.Color("244")).
		Width(10)
	field := func(index int, name string) string {
		marker := " "
		if index == m.focus {
			marker = "›"
		}
		return lipgloss.NewStyle().
			Foreground(lipgloss.Color("220")).
			Render(marker) + " " + label.Render(name) + m.inputs[index].View()
	}

	statusStyle := lipgloss.NewStyle().
		Foreground(lipgloss.Color("196")).
		Align(lipgloss.Center).
		Width(56)
	if m.submitting {
		statusStyle = statusStyle.Foreground(lipgloss.Color("220"))
	}

	content := lipgloss.JoinVertical(
		lipgloss.Left,
		field(0, "Host"),
		field(1, "ID"),
		field(2, "Password"),
		"",
		m.discoveryView(),
		"",
		statusStyle.Render(m.status),
		"",
		lipgloss.NewStyle().
			Foreground(lipgloss.Color("244")).
			Render("Tab/↑/↓ move  Enter login  Ctrl+C quit"),
	)
	box := lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(lipgloss.Color("220")).
		Padding(1, 2).
		Width(62).
		Render(content)

	return lipgloss.Place(
		max(1, width),
		max(1, height),
		lipgloss.Center,
		lipgloss.Center,
		box,
	)
}
