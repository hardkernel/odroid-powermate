package main

import (
	"context"
	"fmt"
	"strings"
	"time"

	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"
)

type uartMenuMode uint8

const (
	uartMenuMain uartMenuMode = iota
	uartMenuBaud
)

type uartMenuState struct {
	mode     uartMenuMode
	selected int
}

type uartMenuItem struct {
	key         string
	label       string
	description string
}

var baudRates = []string{
	"9600", "19200", "38400", "57600", "115200",
	"230400", "460800", "921600", "1500000",
}

func (t *tui) uartMenuItems() []uartMenuItem {
	connection := "UART is reconnecting"
	if t.uart != nil && t.uart.connected.Load() {
		connection = "UART is connected"
	}
	return []uartMenuItem{
		{"g", "Resume terminal", connection},
		{"m", "Toggle MAIN and resume", "Current state: " + plainStateWord(t.switches.Main)},
		{"u", "Toggle USB and resume", "Current state: " + plainStateWord(t.switches.USB)},
		{"p", "Power action", "Requires confirmation"},
		{"r", "Reset action", "Requires confirmation"},
		{"b", "UART baud rate", "Apply a target UART baud rate"},
		{"c", "Clear terminal", "Clear the host screen and local UART log"},
		{"s", "Save raw UART log", "Save the bounded 256 KiB receive buffer"},
		{"x", "Exit terminal", "Return to the Dashboard"},
		{"q", "Quit PowerMate TUI", "Close this program"},
	}
}

func (t *tui) handleUARTMenuKey(key tea.KeyPressMsg) tea.Cmd {
	if t.uartMenu.mode == uartMenuBaud {
		return t.handleUARTBaudKey(key)
	}

	items := t.uartMenuItems()
	switch key.String() {
	case "up", "k":
		t.uartMenu.selected = (t.uartMenu.selected + len(items) - 1) % len(items)
		return nil
	case "down", "j":
		t.uartMenu.selected = (t.uartMenu.selected + 1) % len(items)
		return nil
	case "esc", "g":
		return t.beginUART(nil, false)
	case "ctrl+t":
		return t.beginUART([]byte{uartMenuByte}, false)
	case "m":
		return t.runUARTActionAndResume(
			t.setOutputCmd("MAIN", !t.switches.Main),
		)
	case "u":
		return t.runUARTActionAndResume(
			t.setOutputCmd("USB", !t.switches.USB),
		)
	case "enter":
		return t.activateUARTMenuItem(t.uartMenu.selected)
	}

	for index, item := range items {
		if key.String() == item.key {
			t.uartMenu.selected = index
			return t.activateUARTMenuItem(index)
		}
	}
	return nil
}

func (t *tui) runUARTActionAndResume(action tea.Cmd) tea.Cmd {
	t.uartMenu = uartMenuState{}
	return tea.Batch(
		action,
		t.beginUART(nil, false),
	)
}

func (t *tui) activateUARTMenuItem(index int) tea.Cmd {
	switch index {
	case 0:
		return t.beginUART(nil, false)
	case 1:
		return t.runUARTActionAndResume(
			t.setOutputCmd("MAIN", !t.switches.Main),
		)
	case 2:
		return t.runUARTActionAndResume(
			t.setOutputCmd("USB", !t.switches.USB),
		)
	case 3:
		t.confirm = &confirmation{
			message: "Trigger the Power action?",
			action:  confirmPower,
		}
	case 4:
		t.confirm = &confirmation{
			message: "Trigger the Reset action?",
			action:  confirmReset,
		}
	case 5:
		t.uartMenu.mode = uartMenuBaud
		t.uartMenu.selected = 4
	case 6:
		t.uartLog.Reset()
		return t.beginUART(nil, true)
	case 7:
		return func() tea.Msg {
			filename, err := saveUARTLog(&t.uartLog)
			return uartLogSavedMsg{filename: filename, err: err}
		}
	case 8:
		t.stopUART()
		t.activePage = pageDashboard
		t.uartMenu = uartMenuState{}
	case 9:
		return tea.Quit
	}
	return nil
}

func (t *tui) handleUARTBaudKey(key tea.KeyPressMsg) tea.Cmd {
	switch key.String() {
	case "up", "k":
		t.uartMenu.selected =
			(t.uartMenu.selected + len(baudRates) - 1) % len(baudRates)
	case "down", "j":
		t.uartMenu.selected = (t.uartMenu.selected + 1) % len(baudRates)
	case "esc", "b":
		t.uartMenu.mode = uartMenuMain
		t.uartMenu.selected = 5
	case "enter":
		baudRate := baudRates[t.uartMenu.selected]
		t.uartMenu.mode = uartMenuMain
		t.uartMenu.selected = 5
		return t.setBaudRateCmd(baudRate)
	}
	return nil
}

func (t *tui) setBaudRateCmd(baudRate string) tea.Cmd {
	apiClient := t.client
	ctx := t.ctx
	return func() tea.Msg {
		requestCtx, cancel := context.WithTimeout(ctx, 8*time.Second)
		defer cancel()
		err := apiClient.SetSetting(
			requestCtx,
			map[string]any{"baudrate": baudRate},
		)
		return actionResultMsg{
			success: "UART baud rate set to " + baudRate,
			failure: "Failed to set UART baud rate",
			err:     err,
		}
	}
}

func (t *tui) renderUARTMenu(width, height int) string {
	if t.uartMenu.mode == uartMenuBaud {
		return t.renderUARTBaudMenu(width, height)
	}

	items := t.uartMenuItems()
	lines := make([]string, 0, len(items)+4)
	title := lipgloss.NewStyle().
		Bold(true).
		Foreground(lipgloss.Color("220")).
		Render("UART Menu")
	lines = append(lines, title, "")

	for index, item := range items {
		cursor := "  "
		style := lipgloss.NewStyle()
		if index == t.uartMenu.selected {
			cursor = "› "
			style = style.
				Foreground(lipgloss.Color("232")).
				Background(lipgloss.Color("220")).
				Bold(true)
		}
		line := fmt.Sprintf("%s[%s] %-23s  %s",
			cursor, item.key, item.label, item.description)
		lines = append(lines, style.Render(line))
	}
	lines = append(
		lines,
		"",
		lipgloss.NewStyle().
			Foreground(lipgloss.Color("244")).
			Render("Ctrl+T sends literal Ctrl+T · Esc resumes terminal"),
	)
	if t.notice != "" {
		lines = append(
			lines,
			lipgloss.NewStyle().
				Foreground(lipgloss.Color("220")).
				Render(t.notice),
		)
	}

	panel := lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(lipgloss.Color("220")).
		Padding(1, 2).
		Render(strings.Join(lines, "\n"))
	return lipgloss.Place(
		max(1, width),
		max(1, height),
		lipgloss.Center,
		lipgloss.Center,
		panel,
	)
}

func (t *tui) renderUARTBaudMenu(width, height int) string {
	lines := []string{
		lipgloss.NewStyle().
			Bold(true).
			Foreground(lipgloss.Color("220")).
			Render("UART Baud Rate"),
		"",
	}
	for index, baudRate := range baudRates {
		cursor := "  "
		style := lipgloss.NewStyle()
		if index == t.uartMenu.selected {
			cursor = "› "
			style = style.
				Foreground(lipgloss.Color("232")).
				Background(lipgloss.Color("220")).
				Bold(true)
		}
		lines = append(lines, style.Render(cursor+baudRate))
	}
	lines = append(lines, "", "Esc returns to the UART menu")

	panel := lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(lipgloss.Color("220")).
		Padding(1, 3).
		Render(strings.Join(lines, "\n"))
	return lipgloss.Place(
		max(1, width),
		max(1, height),
		lipgloss.Center,
		lipgloss.Center,
		panel,
	)
}

func plainStateWord(enabled bool) string {
	if enabled {
		return "ON"
	}
	return "OFF"
}
