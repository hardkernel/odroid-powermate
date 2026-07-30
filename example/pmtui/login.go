package main

import (
	"strings"

	"charm.land/bubbles/v2/textinput"
	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"
)

type loginModel struct {
	inputs     [3]textinput.Model
	focus      int
	submitting bool
	status     string
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
		inputs: [3]textinput.Model{host, username, password},
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

func (m *loginModel) Update(msg tea.Msg) (tea.Cmd, bool) {
	if m.submitting {
		return nil, false
	}

	if key, ok := msg.(tea.KeyPressMsg); ok {
		switch key.String() {
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
	m.inputs[m.focus], cmd = m.inputs[m.focus].Update(msg)
	return cmd, false
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
		Width(48)
	if m.submitting {
		statusStyle = statusStyle.Foreground(lipgloss.Color("220"))
	}

	content := lipgloss.JoinVertical(
		lipgloss.Left,
		field(0, "Host"),
		field(1, "ID"),
		field(2, "Password"),
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
		Width(52).
		Render(content)

	return lipgloss.Place(
		max(1, width),
		max(1, height),
		lipgloss.Center,
		lipgloss.Center,
		box,
	)
}
