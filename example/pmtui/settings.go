package main

import (
	"context"
	"errors"
	"fmt"
	"math"
	"net"
	"net/url"
	"strconv"
	"strings"
	"time"

	"charm.land/bubbles/v2/textinput"
	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"
)

type settingsSection uint8

const (
	settingsWiFi settingsSection = iota
	settingsNetwork
	settingsAPMode
	settingsLimits
	settingsUser
	settingsDevice
)

type settingsField uint8

const (
	fieldNone settingsField = iota
	fieldWiFiSSID
	fieldWiFiPassword
	fieldNetworkType
	fieldStaticIP
	fieldGateway
	fieldSubnet
	fieldDNS1
	fieldDNS2
	fieldAPMode
	fieldAPSSID
	fieldAPPassword
	fieldVINLimit
	fieldVINCritical
	fieldMAINLimit
	fieldMAINCritical
	fieldUSBLimit
	fieldUSBCritical
	fieldUsername
	fieldNewPassword
	fieldConfirmPassword
	fieldBaudRate
	fieldPeriod
	fieldRestoreOutput
)

type settingsRowKind uint8

const (
	settingsRowReadOnly settingsRowKind = iota
	settingsRowEdit
	settingsRowChoice
	settingsRowAction
	settingsRowAccessPoint
)

type settingsRow struct {
	label string
	value string
	kind  settingsRowKind
	field settingsField
	index int
}

type settingsModel struct {
	section  settingsSection
	selected int
	loaded   bool
	loading  bool
	applying bool
	scanning bool

	current deviceSettings
	aps     []wifiAccessPoint

	wifiSSID     string
	wifiPassword string

	networkType string
	staticIP    string
	gateway     string
	subnet      string
	dns1        string
	dns2        string

	apMode     string
	apSSID     string
	apPassword string

	vinLimit     string
	vinCritical  string
	mainLimit    string
	mainCritical string
	usbLimit     string
	usbCritical  string

	username        string
	newPassword     string
	confirmPassword string

	baudRate     string
	period       string
	restoreState bool

	editing   bool
	editField settingsField
	editor    textinput.Model

	baudMenuSelected int
	baudMenuOpen     bool
}

type settingsResultMsg struct {
	data deviceSettings
	err  error
}

type wifiScanResultMsg struct {
	aps []wifiAccessPoint
	err error
}

type settingsApplyResultMsg struct {
	section  settingsSection
	username string
	password string
	detail   string
	err      error
}

type rebootResultMsg struct {
	err error
}

var settingsSectionNames = []string{
	"Wi-Fi",
	"Network",
	"AP Mode",
	"Limits",
	"User",
	"Device",
}

func newSettingsModel() settingsModel {
	editor := textinput.New()
	editor.Prompt = ""
	editor.CharLimit = 128
	editor.SetWidth(40)

	return settingsModel{
		networkType: "dhcp",
		apMode:      "sta",
		baudRate:    "115200",
		period:      "1000",
		editor:      editor,
	}
}

func (s *settingsModel) load(data deviceSettings, username string) {
	s.current = data
	s.loaded = true
	s.loading = false

	s.wifiSSID = ""
	s.wifiPassword = ""
	s.networkType = valueOrDefault(data.NetworkType, "dhcp")
	if data.IP != nil {
		s.staticIP = data.IP.IP
		s.gateway = data.IP.Gateway
		s.subnet = data.IP.Subnet
		s.dns1 = data.IP.DNS1
		s.dns2 = data.IP.DNS2
	}
	s.apMode = valueOrDefault(data.Mode, "sta")
	s.apSSID = ""
	s.apPassword = ""
	s.vinLimit = formatSettingFloat(data.VINLimit)
	s.vinCritical = formatSettingFloat(data.VINCriticalLimit)
	s.mainLimit = formatSettingFloat(data.MAINLimit)
	s.mainCritical = formatSettingFloat(data.MAINCriticalLimit)
	s.usbLimit = formatSettingFloat(data.USBLimit)
	s.usbCritical = formatSettingFloat(data.USBCriticalLimit)
	s.username = username
	s.newPassword = ""
	s.confirmPassword = ""
	s.baudRate = valueOrDefault(data.BaudRate, "115200")
	s.period = valueOrDefault(data.Period, "1000")
	s.restoreState = data.RestoreOutputState
	s.editing = false
	s.editField = fieldNone
	s.clampSelection()
}

func formatSettingFloat(value float64) string {
	return strconv.FormatFloat(value, 'f', 1, 64)
}

func maskedSetting(value string) string {
	if value == "" {
		return "(empty)"
	}
	return strings.Repeat("*", min(len(value), 16))
}

func maskedOrOpen(value, emptyLabel string) string {
	if value == "" {
		return "(blank = " + emptyLabel + ")"
	}
	return maskedSetting(value)
}

func (s *settingsModel) rows() []settingsRow {
	switch s.section {
	case settingsWiFi:
		connection := strings.Title(valueOrDefault(s.current.WiFiConnectionStatus, "unknown"))
		if s.current.Connected {
			connection = fmt.Sprintf(
				"Connected to %s (%d dBm)",
				valueOrDefault(s.current.SSID, "unknown"),
				s.current.RSSI,
			)
			if s.current.IP != nil && s.current.IP.IP != "" {
				connection += " · " + s.current.IP.IP
			}
		} else if s.current.WiFiFailureReason != "" {
			connection += " · " + s.current.WiFiFailureReason
		}
		rows := []settingsRow{
			{"Current", connection, settingsRowReadOnly, fieldNone, 0},
			{"SSID", valueOrDefault(s.wifiSSID, "(select or enter)") + "  [1–32 bytes]", settingsRowEdit, fieldWiFiSSID, 0},
			{"Password", maskedOrOpen(s.wifiPassword, "open network") + "  [blank or 8–64]", settingsRowEdit, fieldWiFiPassword, 0},
		}
		for index, ap := range s.aps {
			rows = append(rows, settingsRow{
				label: "  Network",
				value: fmt.Sprintf(
					"%s · %d dBm · %s",
					valueOrDefault(ap.SSID, "<hidden>"),
					ap.RSSI,
					ap.AuthMode,
				),
				kind:  settingsRowAccessPoint,
				index: index,
			})
		}
		return rows
	case settingsNetwork:
		rows := []settingsRow{
			{"Address mode", strings.ToUpper(s.networkType), settingsRowChoice, fieldNetworkType, 0},
		}
		if s.networkType == "static" {
			rows = append(rows,
				settingsRow{"IP address", s.staticIP, settingsRowEdit, fieldStaticIP, 0},
				settingsRow{"Gateway", s.gateway, settingsRowEdit, fieldGateway, 0},
				settingsRow{"Netmask", s.subnet, settingsRowEdit, fieldSubnet, 0},
				settingsRow{"DNS server", s.dns1, settingsRowEdit, fieldDNS1, 0},
				settingsRow{"Backup DNS", valueOrDefault(s.dns2, "(optional)"), settingsRowEdit, fieldDNS2, 0},
			)
		}
		return rows
	case settingsAPMode:
		rows := []settingsRow{
			{"Wi-Fi mode", strings.ToUpper(s.apMode), settingsRowChoice, fieldAPMode, 0},
		}
		if s.apMode == "apsta" {
			rows = append(rows,
				settingsRow{"AP SSID", valueOrDefault(s.apSSID, "(required; not returned)") + "  [1–32 bytes]", settingsRowEdit, fieldAPSSID, 0},
				settingsRow{"AP password", maskedOrOpen(s.apPassword, "open AP") + "  [blank or 8–63]", settingsRowEdit, fieldAPPassword, 0},
			)
		}
		return rows
	case settingsLimits:
		return []settingsRow{
			{"VIN limit", limitValueWithRange(s.vinLimit, s.vinCritical, 10), settingsRowEdit, fieldVINLimit, 0},
			{"VIN critical", criticalValueWithRange(s.vinCritical, s.vinLimit, 15), settingsRowEdit, fieldVINCritical, 0},
			{"MAIN limit", limitValueWithRange(s.mainLimit, s.mainCritical, 10), settingsRowEdit, fieldMAINLimit, 0},
			{"MAIN critical", criticalValueWithRange(s.mainCritical, s.mainLimit, 11), settingsRowEdit, fieldMAINCritical, 0},
			{"USB limit", limitValueWithRange(s.usbLimit, s.usbCritical, 5.9), settingsRowEdit, fieldUSBLimit, 0},
			{"USB critical", criticalValueWithRange(s.usbCritical, s.usbLimit, 6), settingsRowEdit, fieldUSBCritical, 0},
		}
	case settingsUser:
		return []settingsRow{
			{"New ID", s.username, settingsRowEdit, fieldUsername, 0},
			{"New password", maskedSetting(s.newPassword), settingsRowEdit, fieldNewPassword, 0},
			{"Confirm", maskedSetting(s.confirmPassword), settingsRowEdit, fieldConfirmPassword, 0},
		}
	default:
		return []settingsRow{
			{"UART baud rate", s.baudRate, settingsRowChoice, fieldBaudRate, 0},
			{"Sensor period", s.period + " ms  [100–5000, step 100]", settingsRowEdit, fieldPeriod, 0},
			{"Restore VOUT", onOffWord(s.restoreState), settingsRowChoice, fieldRestoreOutput, 0},
			{"Reboot", "Restart PowerMate after 3 seconds", settingsRowAction, fieldNone, 1},
		}
	}
}

func settingFloat(value string, fallback float64) float64 {
	parsed, err := strconv.ParseFloat(value, 64)
	if err != nil {
		return fallback
	}
	return parsed
}

func limitValueWithRange(value, criticalValue string, maximum float64) string {
	return value + " A  " + limitRange(criticalValue, maximum)
}

func limitRange(criticalValue string, maximum float64) string {
	critical := settingFloat(criticalValue, maximum)
	dependentMaximum := min(maximum, critical-0.1)
	if dependentMaximum < 0 {
		dependentMaximum = 0
	}
	return fmt.Sprintf(
		"[0.0–%.1f A; < Critical]",
		dependentMaximum,
	)
}

func criticalValueWithRange(value, limitValue string, maximum float64) string {
	return value + " A  " + criticalRange(limitValue, maximum)
}

func criticalRange(limitValue string, maximum float64) string {
	limit := settingFloat(limitValue, 0)
	minimum := 1.0
	if limit > 0 {
		minimum = max(minimum, limit+0.1)
	}
	return fmt.Sprintf(
		"[%.1f–%.1f A]",
		minimum,
		maximum,
	)
}

func (s *settingsModel) fieldRangeHint(field settingsField) string {
	switch field {
	case fieldWiFiSSID, fieldAPSSID:
		return "[1–32 bytes]"
	case fieldWiFiPassword:
		return "[blank or 8–64]"
	case fieldAPPassword:
		return "[blank or 8–63]"
	case fieldVINLimit:
		return limitRange(s.vinCritical, 10)
	case fieldVINCritical:
		return criticalRange(s.vinLimit, 15)
	case fieldMAINLimit:
		return limitRange(s.mainCritical, 10)
	case fieldMAINCritical:
		return criticalRange(s.mainLimit, 11)
	case fieldUSBLimit:
		return limitRange(s.usbCritical, 5.9)
	case fieldUSBCritical:
		return criticalRange(s.usbLimit, 6)
	case fieldPeriod:
		return "[100–5000 ms, step 100]"
	}
	return ""
}

func onOffWord(enabled bool) string {
	if enabled {
		return "Enabled"
	}
	return "Disabled"
}

func (s *settingsModel) clampSelection() {
	rows := s.rows()
	if len(rows) == 0 {
		s.selected = 0
		return
	}
	s.selected = min(max(0, s.selected), len(rows)-1)
}

func (s *settingsModel) limitWarning() string {
	values := []struct {
		name        string
		value       string
		recommended float64
	}{
		{"VIN", s.vinLimit, 9},
		{"MAIN", s.mainLimit, 7},
		{"USB", s.usbLimit, 4},
	}
	var warnings []string
	for _, item := range values {
		value, err := strconv.ParseFloat(item.value, 64)
		if err != nil {
			continue
		}
		if value == 0 {
			warnings = append(warnings, item.name+" protection disabled")
		} else if value >= item.recommended {
			warnings = append(
				warnings,
				fmt.Sprintf("%s ≥ %.1f A recommended", item.name, item.recommended),
			)
		}
	}
	if len(warnings) == 0 {
		return ""
	}
	return strings.Join(warnings, " · ")
}

func (t *tui) updateSettings(msg tea.Msg) (tea.Cmd, bool) {
	if t.settings.baudMenuOpen {
		key, ok := msg.(tea.KeyPressMsg)
		if !ok {
			return nil, true
		}
		switch key.String() {
		case "ctrl+c":
			return tea.Quit, true
		case "esc":
			t.settings.baudMenuOpen = false
		case "up", "k":
			t.settings.baudMenuSelected =
				(t.settings.baudMenuSelected + len(baudRates) - 1) % len(baudRates)
		case "down", "j":
			t.settings.baudMenuSelected =
				(t.settings.baudMenuSelected + 1) % len(baudRates)
		case "enter":
			t.settings.baudRate = baudRates[t.settings.baudMenuSelected]
			t.settings.baudMenuOpen = false
		}
		return nil, true
	}

	if t.settings.editing {
		if key, ok := msg.(tea.KeyPressMsg); ok {
			switch key.String() {
			case "ctrl+c":
				return tea.Quit, true
			case "esc":
				t.settings.cancelEdit()
				return nil, true
			case "enter":
				t.settings.commitEdit()
				return nil, true
			}
		}
		var cmd tea.Cmd
		t.settings.editor, cmd = t.settings.editor.Update(msg)
		return cmd, true
	}

	key, ok := msg.(tea.KeyPressMsg)
	if !ok {
		return nil, false
	}

	t.notice = ""
	rows := t.settings.rows()
	switch key.String() {
	case "tab", "]":
		t.settings.section = (t.settings.section + 1) % settingsSection(len(settingsSectionNames))
		t.settings.selected = 0
		return nil, true
	case "shift+tab", "[":
		t.settings.section = (t.settings.section + settingsSection(len(settingsSectionNames)) - 1) %
			settingsSection(len(settingsSectionNames))
		t.settings.selected = 0
		return nil, true
	case "up", "k":
		if len(rows) > 0 {
			t.settings.selected = (t.settings.selected + len(rows) - 1) % len(rows)
		}
		return nil, true
	case "down", "j":
		if len(rows) > 0 {
			t.settings.selected = (t.settings.selected + 1) % len(rows)
		}
		return nil, true
	case "left", "h":
		return t.adjustSettingsChoice(-1), true
	case "right":
		return t.adjustSettingsChoice(1), true
	case "s":
		if t.settings.section == settingsWiFi {
			return t.scanWiFiCmd(), true
		}
	case "a":
		return t.applyCurrentSettings(), true
	case "r":
		return t.fetchSettingsCmd(), true
	case "enter":
		if len(rows) == 0 {
			return nil, true
		}
		return t.activateSettingsRow(rows[t.settings.selected]), true
	}
	return nil, false
}

func (t *tui) adjustSettingsChoice(direction int) tea.Cmd {
	rows := t.settings.rows()
	if len(rows) == 0 {
		return nil
	}
	row := rows[t.settings.selected]
	switch row.field {
	case fieldNetworkType:
		if t.settings.networkType == "dhcp" {
			t.settings.networkType = "static"
		} else {
			t.settings.networkType = "dhcp"
		}
	case fieldAPMode:
		if t.settings.apMode == "sta" {
			t.settings.apMode = "apsta"
		} else {
			t.settings.apMode = "sta"
		}
	case fieldBaudRate:
		return nil
	case fieldRestoreOutput:
		t.settings.restoreState = !t.settings.restoreState
	}
	t.settings.clampSelection()
	return nil
}

func (t *tui) activateSettingsRow(row settingsRow) tea.Cmd {
	switch row.kind {
	case settingsRowEdit:
		editWidth := max(16, min(36, t.width-42))
		switch row.field {
		case fieldVINLimit, fieldVINCritical,
			fieldMAINLimit, fieldMAINCritical,
			fieldUSBLimit, fieldUSBCritical, fieldPeriod:
			editWidth = 12
		}
		return t.settings.startEdit(row.field, editWidth)
	case settingsRowChoice:
		if row.field == fieldBaudRate {
			t.settings.openBaudMenu()
			return nil
		}
		return t.adjustSettingsChoice(1)
	case settingsRowAccessPoint:
		if row.index >= 0 && row.index < len(t.settings.aps) {
			t.settings.wifiSSID = t.settings.aps[row.index].SSID
			t.settings.wifiPassword = ""
			t.settings.selected = 2
			t.notice = "Selected " + valueOrDefault(t.settings.wifiSSID, "<hidden SSID>")
			return t.settings.startEdit(fieldWiFiPassword, max(20, min(54, t.width-28)))
		}
	case settingsRowAction:
		return t.activateSettingsAction(row.index)
	}
	return nil
}

func (s *settingsModel) openBaudMenu() {
	s.baudMenuSelected = 0
	for index, baudRate := range baudRates {
		if baudRate == s.baudRate {
			s.baudMenuSelected = index
			break
		}
	}
	s.baudMenuOpen = true
}

func (t *tui) activateSettingsAction(action int) tea.Cmd {
	if t.settings.section == settingsDevice && action == 1 {
		t.confirm = &confirmation{
			message: "Reboot PowerMate in 3 seconds?",
			action:  confirmSettingsReboot,
		}
	}
	return nil
}

func (t *tui) applyCurrentSettings() tea.Cmd {
	if t.settings.applying {
		t.notice = "A settings request is already in progress"
		return nil
	}
	switch t.settings.section {
	case settingsWiFi:
		if t.settings.scanning {
			t.notice = "Wait for the Wi-Fi scan to finish"
			return nil
		}
		if err := t.settings.validateWiFi(); err != nil {
			t.notice = err.Error()
			return nil
		}
		t.confirm = &confirmation{
			message: fmt.Sprintf(
				"Connect PowerMate to %q? The current connection may close.",
				t.settings.wifiSSID,
			),
			action: confirmSettingsWiFi,
		}
	case settingsNetwork:
		if err := t.settings.validateNetwork(); err != nil {
			t.notice = err.Error()
			return nil
		}
		t.confirm = &confirmation{
			message: "Apply network settings? The active IP address may change.",
			action:  confirmSettingsNetwork,
		}
	case settingsAPMode:
		if err := t.settings.validateAPMode(); err != nil {
			t.notice = err.Error()
			return nil
		}
		t.confirm = &confirmation{
			message: "Change Wi-Fi mode? The current connection may close.",
			action:  confirmSettingsAPMode,
		}
	case settingsLimits:
		payload, err := t.settings.limitPayload()
		if err != nil {
			t.notice = err.Error()
			return nil
		}
		return t.applySettingsCmd(settingsLimits, payload, "", "")
	case settingsUser:
		if err := t.settings.validateUser(); err != nil {
			t.notice = err.Error()
			return nil
		}
		t.confirm = &confirmation{
			message: "Change the PowerMate API ID and password?",
			action:  confirmSettingsUser,
		}
	case settingsDevice:
		payload, err := t.settings.devicePayload()
		if err != nil {
			t.notice = err.Error()
			return nil
		}
		return t.applySettingsCmd(settingsDevice, payload, "", "")
	}
	return nil
}

func (s *settingsModel) startEdit(field settingsField, width int) tea.Cmd {
	s.editField = field
	s.editing = true
	s.editor.Reset()
	s.editor.SetWidth(width)
	s.editor.EchoMode = textinput.EchoNormal
	s.editor.EchoCharacter = '*'
	if field == fieldWiFiPassword || field == fieldAPPassword ||
		field == fieldNewPassword || field == fieldConfirmPassword {
		s.editor.EchoMode = textinput.EchoPassword
	}
	s.editor.SetValue(s.fieldValue(field))
	s.editor.CursorEnd()
	return s.editor.Focus()
}

func (s *settingsModel) cancelEdit() {
	s.editing = false
	s.editField = fieldNone
	s.editor.Blur()
}

func (s *settingsModel) commitEdit() {
	value := s.editor.Value()
	if s.editField != fieldWiFiPassword &&
		s.editField != fieldAPPassword &&
		s.editField != fieldNewPassword &&
		s.editField != fieldConfirmPassword {
		value = strings.TrimSpace(value)
	}
	s.setFieldValue(s.editField, value)
	s.cancelEdit()
}

func (s *settingsModel) fieldValue(field settingsField) string {
	switch field {
	case fieldWiFiSSID:
		return s.wifiSSID
	case fieldWiFiPassword:
		return s.wifiPassword
	case fieldStaticIP:
		return s.staticIP
	case fieldGateway:
		return s.gateway
	case fieldSubnet:
		return s.subnet
	case fieldDNS1:
		return s.dns1
	case fieldDNS2:
		return s.dns2
	case fieldAPSSID:
		return s.apSSID
	case fieldAPPassword:
		return s.apPassword
	case fieldVINLimit:
		return s.vinLimit
	case fieldVINCritical:
		return s.vinCritical
	case fieldMAINLimit:
		return s.mainLimit
	case fieldMAINCritical:
		return s.mainCritical
	case fieldUSBLimit:
		return s.usbLimit
	case fieldUSBCritical:
		return s.usbCritical
	case fieldUsername:
		return s.username
	case fieldNewPassword:
		return s.newPassword
	case fieldConfirmPassword:
		return s.confirmPassword
	case fieldPeriod:
		return s.period
	}
	return ""
}

func (s *settingsModel) setFieldValue(field settingsField, value string) {
	switch field {
	case fieldWiFiSSID:
		s.wifiSSID = value
	case fieldWiFiPassword:
		s.wifiPassword = value
	case fieldStaticIP:
		s.staticIP = value
	case fieldGateway:
		s.gateway = value
	case fieldSubnet:
		s.subnet = value
	case fieldDNS1:
		s.dns1 = value
	case fieldDNS2:
		s.dns2 = value
	case fieldAPSSID:
		s.apSSID = value
	case fieldAPPassword:
		s.apPassword = value
	case fieldVINLimit:
		s.vinLimit = value
	case fieldVINCritical:
		s.vinCritical = value
	case fieldMAINLimit:
		s.mainLimit = value
	case fieldMAINCritical:
		s.mainCritical = value
	case fieldUSBLimit:
		s.usbLimit = value
	case fieldUSBCritical:
		s.usbCritical = value
	case fieldUsername:
		s.username = value
	case fieldNewPassword:
		s.newPassword = value
	case fieldConfirmPassword:
		s.confirmPassword = value
	case fieldPeriod:
		s.period = value
	}
}

func (s *settingsModel) validateWiFi() error {
	if strings.TrimSpace(s.wifiSSID) == "" {
		return fmt.Errorf("Wi-Fi SSID is required")
	}
	if len(s.wifiSSID) > 32 {
		return fmt.Errorf("Wi-Fi SSID must be at most 32 bytes")
	}
	if length := len(s.wifiPassword); length > 0 && (length < 8 || length > 64) {
		return fmt.Errorf("Wi-Fi password must be empty or 8-64 characters")
	}
	return nil
}

func (s *settingsModel) validateNetwork() error {
	if s.networkType == "dhcp" {
		return nil
	}
	values := []struct {
		name  string
		value string
	}{
		{"IP address", s.staticIP},
		{"gateway", s.gateway},
		{"netmask", s.subnet},
		{"DNS server", s.dns1},
	}
	for _, item := range values {
		if !validIPv4(item.value) {
			return fmt.Errorf("%s is not a valid IP address", item.name)
		}
	}
	if !validIPv4Netmask(s.subnet) {
		return fmt.Errorf("netmask is not a contiguous IPv4 netmask")
	}
	if s.dns2 != "" && !validIPv4(s.dns2) {
		return fmt.Errorf("backup DNS is not a valid IP address")
	}
	return nil
}

func validIPv4(value string) bool {
	ip := net.ParseIP(value)
	return ip != nil && ip.To4() != nil
}

func validIPv4Netmask(value string) bool {
	ip := net.ParseIP(value)
	if ip == nil || ip.To4() == nil {
		return false
	}
	ones, bits := net.IPMask(ip.To4()).Size()
	return bits == 32 && ones >= 0
}

func (s *settingsModel) validateAPMode() error {
	if s.apMode == "sta" {
		return nil
	}
	if strings.TrimSpace(s.apSSID) == "" {
		return fmt.Errorf("AP SSID is required in APSTA mode")
	}
	if len(s.apSSID) > 32 {
		return fmt.Errorf("AP SSID must be at most 32 bytes")
	}
	if length := len(s.apPassword); length > 0 && (length < 8 || length > 63) {
		return fmt.Errorf("AP password must be empty or 8-63 characters")
	}
	return nil
}

func (s *settingsModel) validateUser() error {
	if strings.TrimSpace(s.username) == "" {
		return fmt.Errorf("new ID is required")
	}
	if s.newPassword == "" {
		return fmt.Errorf("new password is required")
	}
	if s.newPassword != s.confirmPassword {
		return fmt.Errorf("new password and confirmation do not match")
	}
	return nil
}

func parseLimit(name, value string, minimum, maximum float64) (float64, error) {
	parsed, err := strconv.ParseFloat(value, 64)
	if err != nil || parsed < minimum || parsed > maximum {
		return 0, fmt.Errorf("%s must be %.1f-%.1f A", name, minimum, maximum)
	}
	if math.Abs(parsed*10-math.Round(parsed*10)) > 1e-6 {
		return 0, fmt.Errorf("%s must use 0.1 A steps", name)
	}
	return parsed, nil
}

func (s *settingsModel) limitPayload() (map[string]any, error) {
	vin, err := parseLimit("VIN limit", s.vinLimit, 0, 10)
	if err != nil {
		return nil, err
	}
	vinCritical, err := parseLimit("VIN critical limit", s.vinCritical, 1, 15)
	if err != nil {
		return nil, err
	}
	mainLimit, err := parseLimit("MAIN limit", s.mainLimit, 0, 10)
	if err != nil {
		return nil, err
	}
	mainCritical, err := parseLimit("MAIN critical limit", s.mainCritical, 1, 11)
	if err != nil {
		return nil, err
	}
	usb, err := parseLimit("USB limit", s.usbLimit, 0, 5.9)
	if err != nil {
		return nil, err
	}
	usbCritical, err := parseLimit("USB critical limit", s.usbCritical, 1, 6)
	if err != nil {
		return nil, err
	}
	for _, pair := range []struct {
		name     string
		limit    float64
		critical float64
	}{
		{"VIN", vin, vinCritical},
		{"MAIN", mainLimit, mainCritical},
		{"USB", usb, usbCritical},
	} {
		if pair.limit > 0 && pair.limit >= pair.critical {
			return nil, fmt.Errorf("%s Limit must be lower than Critical Limit", pair.name)
		}
	}
	return map[string]any{
		"vin_current_limit":           vin,
		"vin_critical_current_limit":  vinCritical,
		"main_current_limit":          mainLimit,
		"main_critical_current_limit": mainCritical,
		"usb_current_limit":           usb,
		"usb_critical_current_limit":  usbCritical,
	}, nil
}

func (s *settingsModel) devicePayload() (map[string]any, error) {
	period, err := strconv.Atoi(s.period)
	if err != nil || period < 100 || period > 5000 || period%100 != 0 {
		return nil, fmt.Errorf("sensor period must be 100-5000 ms in 100 ms steps")
	}
	validBaud := false
	for _, baudRate := range baudRates {
		if baudRate == s.baudRate {
			validBaud = true
			break
		}
	}
	if !validBaud {
		return nil, fmt.Errorf("unsupported UART baud rate")
	}
	return map[string]any{
		"baudrate":             s.baudRate,
		"period":               s.period,
		"restore_output_state": s.restoreState,
	}, nil
}

func (s *settingsModel) payload(section settingsSection) map[string]any {
	switch section {
	case settingsWiFi:
		return map[string]any{
			"ssid":     s.wifiSSID,
			"password": s.wifiPassword,
		}
	case settingsNetwork:
		payload := map[string]any{"net_type": s.networkType}
		if s.networkType == "static" {
			payload["ip"] = s.staticIP
			payload["gateway"] = s.gateway
			payload["subnet"] = s.subnet
			payload["dns1"] = s.dns1
			if s.dns2 != "" {
				payload["dns2"] = s.dns2
			}
		}
		return payload
	case settingsAPMode:
		payload := map[string]any{"mode": s.apMode}
		if s.apMode == "apsta" {
			payload["ap_ssid"] = s.apSSID
			if s.apPassword != "" {
				payload["ap_password"] = s.apPassword
			}
		}
		return payload
	case settingsUser:
		return map[string]any{
			"new_username": s.username,
			"new_password": s.newPassword,
		}
	}
	return nil
}

func (t *tui) fetchSettingsCmd() tea.Cmd {
	if t.settings.loading || t.settings.applying {
		return nil
	}
	t.settings.loading = true
	t.notice = "Loading settings..."
	apiClient := t.client
	ctx := t.ctx
	return func() tea.Msg {
		requestCtx, cancel := context.WithTimeout(ctx, 8*time.Second)
		defer cancel()
		data, err := apiClient.GetSettings(requestCtx)
		return settingsResultMsg{data: data, err: err}
	}
}

func (t *tui) scanWiFiCmd() tea.Cmd {
	if t.settings.scanning || t.settings.applying {
		return nil
	}
	t.settings.scanning = true
	t.notice = "Scanning for Wi-Fi access points..."
	apiClient := t.client
	ctx := t.ctx
	return func() tea.Msg {
		requestCtx, cancel := context.WithTimeout(ctx, 15*time.Second)
		defer cancel()
		aps, err := apiClient.ScanWiFi(requestCtx)
		return wifiScanResultMsg{aps: aps, err: err}
	}
}

func (t *tui) applySettingsCmd(
	section settingsSection,
	payload map[string]any,
	username string,
	password string,
) tea.Cmd {
	if t.settings.applying {
		return nil
	}
	t.settings.applying = true
	t.notice = "Applying " + settingsSectionNames[section] + " settings..."
	apiClient := t.client
	ctx := t.ctx
	return func() tea.Msg {
		timeout := 12 * time.Second
		if section == settingsWiFi {
			timeout = 25 * time.Second
		}
		requestCtx, cancel := context.WithTimeout(ctx, timeout)
		defer cancel()
		err := apiClient.SetSetting(requestCtx, payload)
		if err != nil && settingRequestMayHaveApplied(section, err) {
			return settingsApplyResultMsg{
				section: section,
				detail:  "The connection was interrupted; the setting may have been applied. Reconnect and verify.",
			}
		}
		if err == nil && section == settingsWiFi {
			detail, connectionErr := waitForWiFiResult(requestCtx, apiClient)
			return settingsApplyResultMsg{
				section: section,
				detail:  detail,
				err:     connectionErr,
			}
		}
		return settingsApplyResultMsg{
			section:  section,
			username: username,
			password: password,
			err:      err,
		}
	}
}

func settingRequestMayHaveApplied(section settingsSection, err error) bool {
	if section != settingsWiFi && section != settingsNetwork && section != settingsAPMode {
		return false
	}
	var urlError *url.Error
	return errors.As(err, &urlError) || errors.Is(err, context.DeadlineExceeded)
}

func waitForWiFiResult(ctx context.Context, apiClient *client) (string, error) {
	ticker := time.NewTicker(time.Second)
	defer ticker.Stop()
	hadRequestError := false

	for {
		select {
		case <-ctx.Done():
			if hadRequestError {
				return "Connection result unavailable because the active network path changed.", nil
			}
			return "", fmt.Errorf("timed out waiting for the Wi-Fi connection result")
		case <-ticker.C:
			status, err := apiClient.GetSettings(ctx)
			if err != nil {
				hadRequestError = true
				continue
			}
			if status.WiFiConnectionStatus == "failed" {
				return "", fmt.Errorf(
					"Wi-Fi connection failed: %s",
					valueOrDefault(status.WiFiFailureReason, "UNKNOWN"),
				)
			}
			if status.WiFiConnectionStatus == "connected" && status.Connected {
				address := "an IP address"
				if status.IP != nil && status.IP.IP != "" {
					address = status.IP.IP
				}
				return fmt.Sprintf("Connected to %q at %s", status.SSID, address), nil
			}
		}
	}
}

func (t *tui) rebootCmd() tea.Cmd {
	apiClient := t.client
	ctx := t.ctx
	return func() tea.Msg {
		requestCtx, cancel := context.WithTimeout(ctx, 8*time.Second)
		defer cancel()
		return rebootResultMsg{err: apiClient.Reboot(requestCtx)}
	}
}

func (t *tui) renderSettings(width, height int) string {
	categoryParts := make([]string, 0, len(settingsSectionNames))
	for index, name := range settingsSectionNames {
		style := lipgloss.NewStyle().Foreground(lipgloss.Color("244"))
		if settingsSection(index) == t.settings.section {
			style = style.
				Foreground(lipgloss.Color("232")).
				Background(lipgloss.Color("220")).
				Bold(true)
		}
		categoryParts = append(categoryParts, style.Render(" "+name+" "))
	}
	categoryLine := lipgloss.NewStyle().
		Width(max(1, width-2)).
		Align(lipgloss.Center).
		Render(strings.Join(categoryParts, " "))
	actionLine := lipgloss.NewStyle().
		Width(max(1, width-2)).
		Align(lipgloss.Center).
		Foreground(lipgloss.Color("220")).
		Bold(true).
		Render(t.settings.actionHint())

	if t.settings.baudMenuOpen {
		return t.renderSettingsBaudMenu(width, height, categoryLine)
	}

	if t.settings.loading && !t.settings.loaded {
		return lipgloss.NewStyle().
			Border(lipgloss.NormalBorder()).
			BorderForeground(lipgloss.Color("240")).
			Width(max(1, width)).
			Height(max(1, height)).
			Render(lipgloss.JoinVertical(lipgloss.Left, categoryLine, actionLine, "", "Loading settings..."))
	}

	rows := t.settings.rows()
	availableRows := max(1, height-6)
	if t.settings.editing {
		availableRows = max(1, height-8)
	}
	start := 0
	if len(rows) > availableRows && t.settings.selected >= availableRows {
		start = t.settings.selected - availableRows + 1
	}
	end := min(len(rows), start+availableRows)

	labelWidth := 18
	if width < 64 {
		labelWidth = 14
	}
	lines := []string{categoryLine, actionLine, ""}
	for index := start; index < end; index++ {
		row := rows[index]
		cursor := "  "
		style := lipgloss.NewStyle()
		if index == t.settings.selected {
			cursor = "› "
			style = style.
				Foreground(lipgloss.Color("232")).
				Background(lipgloss.Color("220")).
				Bold(true)
		}
		value := row.value
		if t.settings.editing && row.field == t.settings.editField {
			value = t.settings.editor.View()
			if hint := t.settings.fieldRangeHint(row.field); hint != "" {
				value += "  " + hint
			}
		} else {
			value = truncateSettingValue(value, max(8, width-labelWidth-7))
		}
		line := cursor + lipgloss.NewStyle().
			Width(labelWidth).
			Render(row.label) + value
		lines = append(lines, style.Render(line))
	}
	if len(rows) > availableRows {
		lines = append(lines, fmt.Sprintf("  Rows %d-%d of %d", start+1, end, len(rows)))
	}
	if t.settings.editing {
		lines = append(lines, "", "Enter save · Esc cancel")
	}
	if t.settings.section == settingsLimits {
		lines = append(
			lines,
			"",
			lipgloss.NewStyle().
				Foreground(lipgloss.Color("244")).
				Width(max(1, width-4)).
				Render("0 disables Limit · recommended continuous: VIN <9 A · MAIN <7 A · USB <4 A"),
		)
		if warning := t.settings.limitWarning(); warning != "" {
			lines = append(
				lines,
				lipgloss.NewStyle().
					Foreground(lipgloss.Color("214")).
					Bold(true).
					Width(max(1, width-4)).
					Render("Warning: "+warning),
			)
		}
	}

	return lipgloss.NewStyle().
		Border(lipgloss.NormalBorder()).
		BorderForeground(lipgloss.Color("240")).
		Width(max(1, width)).
		Height(max(1, height)).
		MaxWidth(max(1, width)).
		MaxHeight(max(1, height)).
		Render(lipgloss.JoinVertical(lipgloss.Left, lines...))
}

func (s *settingsModel) actionHint() string {
	switch s.section {
	case settingsWiFi:
		if s.scanning {
			return "[s] Scanning...   [a] Connect"
		}
		return "[s] Scan   [a] Connect"
	case settingsDevice:
		return "[a] Apply"
	default:
		return "[a] Apply"
	}
}

func (t *tui) renderSettingsBaudMenu(
	width int,
	height int,
	categoryLine string,
) string {
	lines := []string{
		categoryLine,
		lipgloss.NewStyle().
			Width(max(1, width-2)).
			Align(lipgloss.Center).
			Foreground(lipgloss.Color("220")).
			Bold(true).
			Render("UART baud rate"),
		"",
	}
	for index, baudRate := range baudRates {
		cursor := "  "
		style := lipgloss.NewStyle()
		if index == t.settings.baudMenuSelected {
			cursor = "› "
			style = style.
				Foreground(lipgloss.Color("232")).
				Background(lipgloss.Color("220")).
				Bold(true)
		}
		lines = append(lines, style.Render(cursor+baudRate))
	}
	return lipgloss.NewStyle().
		Border(lipgloss.NormalBorder()).
		BorderForeground(lipgloss.Color("240")).
		Width(max(1, width)).
		Height(max(1, height)).
		MaxWidth(max(1, width)).
		MaxHeight(max(1, height)).
		Render(lipgloss.JoinVertical(lipgloss.Left, lines...))
}

func truncateSettingValue(value string, width int) string {
	runes := []rune(value)
	if len(runes) <= width {
		return value
	}
	if width <= 1 {
		return "…"
	}
	return string(runes[:width-1]) + "…"
}
