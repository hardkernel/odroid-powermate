package main

import (
	"context"
	"fmt"
	"strings"
	"sync"
	"sync/atomic"
	"time"

	"charm.land/bubbles/v2/viewport"
	tea "charm.land/bubbletea/v2"
	"charm.land/lipgloss/v2"
)

const (
	maxEventLines    = 500
	mainReservedRows = 4
)

type page uint8

const (
	pageDashboard page = iota
	pageEvents
	pageDebug
	pageUART
	pageSettings
)

type confirmAction uint8

const (
	confirmPower confirmAction = iota
	confirmReset
	confirmClearEvents
	confirmSettingsWiFi
	confirmSettingsNetwork
	confirmSettingsAPMode
	confirmSettingsUser
	confirmSettingsReboot
)

type confirmation struct {
	message  string
	action   confirmAction
	selected bool
}

type loginResultMsg struct {
	client *client
	err    error
}

type versionResultMsg struct {
	version string
	err     error
}

type controlResultMsg struct {
	status controlStatus
	err    error
}

type controlSetResultMsg struct {
	output  string
	enabled bool
	err     error
}

type diagnosticsResultMsg struct {
	data diagnostics
	err  error
}

type actionResultMsg struct {
	success string
	failure string
	err     error
}

type statusSensorReadyMsg struct{}

type statusPayloadMsg struct {
	status statusMessage
}

type statusStateMsg struct {
	connected bool
	err       error
}

type debugTickMsg struct {
	epoch uint64
}

type uartExitedMsg struct {
	reason uartExitReason
	err    error
}

type uartLogSavedMsg struct {
	filename string
	err      error
}

type tui struct {
	ctx    context.Context
	cancel context.CancelFunc

	width  int
	height int

	login         loginModel
	authenticated bool
	openUART      bool
	client        *client
	activePage    page
	version       string
	notice        string

	wsOnline    bool
	wifi        wifiStatus
	switches    switchStatus
	lastSensor  sensorData
	graphs      graphHistory
	graphLayout graphLayout

	eventsView viewport.Model
	debugView  viewport.Model
	eventLines []string
	diagnostic diagnostics
	haveDebug  bool

	confirm *confirmation

	recorder recorder
	uartLog  uartLog
	uart     *uartBridge
	uartMenu uartMenuState
	settings settingsModel

	statusCh     chan tea.Msg
	sensorMu     sync.Mutex
	latest       sensorData
	sensorQueued atomic.Bool

	lastStatusUnixMS atomic.Int64
	debugEpoch       uint64
	debugInFlight    bool
}

func newTUI(
	defaultHost string,
	defaultUsername string,
	openUART bool,
) *tui {
	ctx, cancel := context.WithCancel(context.Background())
	events := viewport.New(
		viewport.WithWidth(1),
		viewport.WithHeight(1),
	)
	events.SoftWrap = true
	debug := viewport.New(
		viewport.WithWidth(1),
		viewport.WithHeight(1),
	)
	debug.SoftWrap = true

	return &tui{
		ctx:        ctx,
		cancel:     cancel,
		login:      newLoginModel(defaultHost, defaultUsername),
		openUART:   openUART,
		activePage: pageDashboard,
		version:    "unknown",
		eventsView: events,
		debugView:  debug,
		settings:   newSettingsModel(),
		statusCh:   make(chan tea.Msg, 256),
	}
}

func (t *tui) Run() error {
	_, err := tea.NewProgram(t).Run()
	t.cancel()
	t.stopUART()
	if t.recorder.Active() {
		_, _ = t.recorder.Stop()
	}
	return err
}

func (t *tui) Init() tea.Cmd {
	return t.login.Init()
}

func (t *tui) Update(msg tea.Msg) (tea.Model, tea.Cmd) {
	if size, ok := msg.(tea.WindowSizeMsg); ok {
		t.width = size.Width
		t.height = size.Height
		t.resizeViewports()
		return t, nil
	}

	if !t.authenticated {
		return t.updateLogin(msg)
	}

	switch message := msg.(type) {
	case statusSensorReadyMsg:
		t.sensorQueued.Store(false)
		t.sensorMu.Lock()
		sensor := t.latest
		t.sensorMu.Unlock()
		t.applySensor(sensor)
		return t, t.waitStatusCmd()
	case statusPayloadMsg:
		t.applyStatus(message.status)
		return t, t.waitStatusCmd()
	case statusStateMsg:
		t.wsOnline = message.connected
		if message.err != nil {
			t.notice = "Status WebSocket: " + message.err.Error()
		}
		return t, t.waitStatusCmd()
	case versionResultMsg:
		if message.err != nil {
			t.notice = "Version query failed: " + message.err.Error()
		} else {
			t.version = message.version
		}
	case controlResultMsg:
		if message.err != nil {
			t.notice = "Control query failed: " + message.err.Error()
		} else {
			t.switches = switchStatus{
				Main: message.status.Main,
				USB:  message.status.USB,
			}
		}
	case controlSetResultMsg:
		if message.err != nil {
			t.notice = message.output + " control failed: " + message.err.Error()
			return t, nil
		}
		if message.output == "MAIN" {
			t.switches.Main = message.enabled
		} else {
			t.switches.USB = message.enabled
		}
		t.notice = fmt.Sprintf(
			"%s output is now %s",
			message.output,
			plainStateWord(message.enabled),
		)
		return t, t.fetchControlCmd()
	case diagnosticsResultMsg:
		t.debugInFlight = false
		if message.err != nil {
			t.notice = "Diagnostics query failed: " + message.err.Error()
		} else {
			t.diagnostic = message.data
			t.haveDebug = true
			t.updateDebugContent()
		}
	case debugTickMsg:
		if t.activePage == pageDebug && message.epoch == t.debugEpoch {
			return t, tea.Batch(
				t.fetchDiagnosticsCmd(),
				t.debugTickCmd(message.epoch),
			)
		}
	case actionResultMsg:
		if message.err != nil {
			t.notice = message.failure + ": " + message.err.Error()
		} else {
			t.notice = message.success
		}
	case settingsResultMsg:
		t.settings.loading = false
		if message.err != nil {
			t.notice = "Settings query failed: " + message.err.Error()
		} else {
			t.settings.load(message.data, t.client.Username())
			t.notice = "Settings refreshed"
		}
	case wifiScanResultMsg:
		t.settings.scanning = false
		if message.err != nil {
			t.notice = "Wi-Fi scan failed: " + message.err.Error()
		} else {
			t.settings.aps = message.aps
			t.settings.clampSelection()
			t.notice = fmt.Sprintf("Wi-Fi scan found %d access points", len(message.aps))
		}
	case settingsApplyResultMsg:
		t.settings.applying = false
		if message.err != nil {
			t.notice = "Failed to apply " +
				settingsSectionNames[message.section] +
				" settings: " + message.err.Error()
			break
		}
		if message.username != "" {
			t.client.SetCredentials(message.username, message.password)
			t.settings.newPassword = ""
			t.settings.confirmPassword = ""
			t.notice = "API credentials updated; subsequent requests will re-authenticate"
			return t, t.fetchSettingsCmd()
		}
		if message.detail != "" {
			t.notice = message.detail
			break
		}
		switch message.section {
		case settingsWiFi:
			t.notice = "Wi-Fi connection initiated; the current connection may close"
		case settingsNetwork:
			t.notice = "Network settings applied; reconnect if the address changed"
		case settingsAPMode:
			t.notice = "Wi-Fi mode change initiated; reconnect after reconfiguration"
		default:
			t.notice = settingsSectionNames[message.section] + " settings applied"
			return t, t.fetchSettingsCmd()
		}
	case rebootResultMsg:
		if message.err != nil {
			t.notice = "Reboot request failed: " + message.err.Error()
		} else {
			t.notice = "Reboot scheduled in 3 seconds"
		}
	case uartExitedMsg:
		t.activePage = pageUART
		t.uartMenu = uartMenuState{}
		if message.err != nil {
			t.notice = "UART terminal stopped: " + message.err.Error()
		} else if message.reason == uartExitEOF {
			t.notice = "UART terminal input closed"
		}
	case uartLogSavedMsg:
		if message.err != nil {
			t.notice = "Failed to save UART log: " + message.err.Error()
		} else {
			t.notice = "UART log saved as " + message.filename
		}
	}

	if t.activePage == pageSettings && t.confirm == nil {
		if cmd, handled := t.updateSettings(msg); handled {
			return t, cmd
		}
	}

	if key, ok := msg.(tea.KeyPressMsg); ok {
		if t.confirm != nil {
			return t, t.handleConfirmationKey(key)
		}
		if t.activePage == pageUART {
			return t, t.handleUARTMenuKey(key)
		}
		if cmd, handled := t.handleGlobalKey(key); handled {
			return t, cmd
		}
	}

	var cmd tea.Cmd
	switch t.activePage {
	case pageEvents:
		t.eventsView, cmd = t.eventsView.Update(msg)
	case pageDebug:
		t.debugView, cmd = t.debugView.Update(msg)
	}
	return t, cmd
}

func (t *tui) updateLogin(msg tea.Msg) (tea.Model, tea.Cmd) {
	if result, ok := msg.(loginResultMsg); ok {
		if result.err != nil {
			return t, t.login.Failed(result.err)
		}

		t.client = result.client
		t.authenticated = true
		t.login.inputs[2].Reset()
		go t.client.RunStatus(
			t.ctx,
			t.enqueueStatus,
			t.enqueueStatusState,
		)
		commands := []tea.Cmd{
			t.waitStatusCmd(),
			t.fetchVersionCmd(),
			t.fetchControlCmd(),
		}
		if t.openUART {
			t.openUART = false
			t.activePage = pageUART
			commands = append(commands, t.beginUART(nil, false))
		}
		return t, tea.Batch(commands...)
	}

	if key, ok := msg.(tea.KeyPressMsg); ok &&
		key.String() == "ctrl+c" {
		return t, tea.Quit
	}

	cmd, submit := t.login.Update(msg)
	if !submit {
		return t, cmd
	}

	host, username, password := t.login.Credentials()
	if host == "" || username == "" || password == "" {
		return t, t.login.Failed(
			fmt.Errorf("host, ID, and password are required"),
		)
	}
	t.login.Begin()
	return t, t.loginCmd(host, username, password)
}

func (t *tui) loginCmd(host, username, password string) tea.Cmd {
	ctx := t.ctx
	return func() tea.Msg {
		apiClient, err := newClient(host, username, password)
		if err != nil {
			return loginResultMsg{err: err}
		}
		requestCtx, cancel := context.WithTimeout(ctx, 8*time.Second)
		defer cancel()
		if err := apiClient.Login(requestCtx); err != nil {
			return loginResultMsg{err: err}
		}
		return loginResultMsg{client: apiClient}
	}
}

func (t *tui) enqueueStatus(message statusMessage) {
	if message.Kind == payloadSensor {
		if err := t.recorder.Write(message.Sensor); err != nil {
			select {
			case t.statusCh <- actionResultMsg{
				failure: "CSV write failed",
				err:     err,
			}:
			default:
			}
		}
		t.sensorMu.Lock()
		t.latest = message.Sensor
		t.sensorMu.Unlock()
		if !t.sensorQueued.CompareAndSwap(false, true) {
			return
		}
		select {
		case t.statusCh <- statusSensorReadyMsg{}:
		default:
			t.sensorQueued.Store(false)
		}
		return
	}

	select {
	case t.statusCh <- statusPayloadMsg{status: message}:
	default:
	}
}

func (t *tui) enqueueStatusState(connected bool, err error) {
	select {
	case t.statusCh <- statusStateMsg{connected: connected, err: err}:
	default:
	}
}

func (t *tui) waitStatusCmd() tea.Cmd {
	ctx := t.ctx
	statusCh := t.statusCh
	return func() tea.Msg {
		select {
		case <-ctx.Done():
			return nil
		case message := <-statusCh:
			return message
		}
	}
}

func (t *tui) applySensor(sensor sensorData) {
	t.lastStatusUnixMS.Store(time.Now().UnixMilli())
	t.lastSensor = sensor
	t.graphs.Add(sensor)
}

func (t *tui) applyStatus(message statusMessage) {
	t.lastStatusUnixMS.Store(time.Now().UnixMilli())
	switch message.Kind {
	case payloadWiFi:
		t.wifi = message.WiFi
	case payloadSwitch:
		t.switches = message.Switch
	case payloadEvent:
		t.appendEvent(message.Event)
	}
}

func (t *tui) appendEvent(event eventData) {
	levels := []string{"INFO", "WARNING", "CRITICAL", "FATAL"}
	level := fmt.Sprintf("LEVEL-%d", event.Level)
	if event.Level >= 0 && int(event.Level) < len(levels) {
		level = levels[event.Level]
	}

	timestamp := "time-unavailable"
	if event.TimestampMS > 0 {
		timestamp = time.UnixMilli(int64(event.TimestampMS)).
			Local().
			Format("2006-01-02 15:04:05")
	}
	line := fmt.Sprintf(
		"%s  %-8s  up=%s  %s",
		timestamp,
		level,
		formatDurationMS(event.UptimeMS),
		event.Message,
	)
	t.eventLines = append(t.eventLines, line)
	if len(t.eventLines) > maxEventLines {
		t.eventLines = append(
			[]string(nil),
			t.eventLines[len(t.eventLines)-maxEventLines:]...,
		)
	}
	t.eventsView.SetContent(strings.Join(t.eventLines, "\n"))
	t.eventsView.GotoBottom()
}

func (t *tui) handleGlobalKey(key tea.KeyPressMsg) (tea.Cmd, bool) {
	t.notice = ""
	switch key.String() {
	case "ctrl+c", "q":
		return tea.Quit, true
	case "1":
		return t.activatePage(pageDashboard), true
	case "2":
		return t.activatePage(pageEvents), true
	case "3":
		return t.activatePage(pageDebug), true
	case "4":
		t.activePage = pageUART
		return t.beginUART(nil, false), true
	case "5":
		return t.activatePage(pageSettings), true
	case "m":
		return t.setOutputCmd("MAIN", !t.switches.Main), true
	case "u":
		return t.setOutputCmd("USB", !t.switches.USB), true
	case "p":
		t.confirm = &confirmation{
			message: "Trigger the Power action?",
			action:  confirmPower,
		}
		return nil, true
	case "x":
		t.confirm = &confirmation{
			message: "Trigger the Reset action?",
			action:  confirmReset,
		}
		return nil, true
	case "c":
		t.toggleRecording()
		return nil, true
	case "l":
		t.toggleGraphLayout()
		return nil, true
	case "e":
		if t.activePage == pageEvents {
			t.confirm = &confirmation{
				message: "Clear events shown in this TUI?",
				action:  confirmClearEvents,
			}
			return nil, true
		}
	case "r":
		if t.activePage == pageDebug {
			return t.fetchDiagnosticsCmd(), true
		}
		return t.fetchControlCmd(), true
	}
	return nil, false
}

func (t *tui) toggleGraphLayout() {
	contentHeight := max(1, t.height-mainReservedRows)
	statusHeight := lipgloss.Height(t.renderDashboardStatus(max(1, t.width)))
	graphHeight := max(1, contentHeight-statusHeight)
	current := resolveGraphLayout(
		t.graphLayout,
		max(1, t.width),
		graphHeight,
	)
	if current == graphLayoutHorizontal {
		t.graphLayout = graphLayoutVertical
		return
	}
	t.graphLayout = graphLayoutHorizontal
}

func (t *tui) activatePage(next page) tea.Cmd {
	if t.activePage == pageDebug && next != pageDebug {
		t.debugEpoch++
	}
	t.activePage = next
	if next == pageDebug {
		t.debugEpoch++
		return tea.Batch(
			t.fetchDiagnosticsCmd(),
			t.debugTickCmd(t.debugEpoch),
		)
	}
	if next == pageSettings {
		return t.fetchSettingsCmd()
	}
	return nil
}

func (t *tui) handleConfirmationKey(key tea.KeyPressMsg) tea.Cmd {
	switch key.String() {
	case "esc", "n":
		t.confirm = nil
	case "left", "right", "tab", "shift+tab":
		t.confirm.selected = !t.confirm.selected
	case "y":
		t.confirm.selected = true
		return t.executeConfirmation()
	case "enter":
		if t.confirm.selected {
			return t.executeConfirmation()
		}
		t.confirm = nil
	}
	return nil
}

func (t *tui) executeConfirmation() tea.Cmd {
	action := t.confirm.action
	t.confirm = nil
	switch action {
	case confirmPower:
		return t.postActionCmd("power_trigger", "Power action triggered")
	case confirmReset:
		return t.postActionCmd("reset_trigger", "Reset action triggered")
	case confirmClearEvents:
		t.eventLines = nil
		t.eventsView.SetContent("")
		t.notice = "Events cleared from this TUI"
	case confirmSettingsWiFi:
		return t.applySettingsCmd(
			settingsWiFi,
			t.settings.payload(settingsWiFi),
			"",
			"",
		)
	case confirmSettingsNetwork:
		return t.applySettingsCmd(
			settingsNetwork,
			t.settings.payload(settingsNetwork),
			"",
			"",
		)
	case confirmSettingsAPMode:
		return t.applySettingsCmd(
			settingsAPMode,
			t.settings.payload(settingsAPMode),
			"",
			"",
		)
	case confirmSettingsUser:
		return t.applySettingsCmd(
			settingsUser,
			t.settings.payload(settingsUser),
			t.settings.username,
			t.settings.newPassword,
		)
	case confirmSettingsReboot:
		return t.rebootCmd()
	}
	return nil
}

func (t *tui) fetchVersionCmd() tea.Cmd {
	apiClient := t.client
	ctx := t.ctx
	return func() tea.Msg {
		requestCtx, cancel := context.WithTimeout(ctx, 8*time.Second)
		defer cancel()
		version, err := apiClient.GetVersion(requestCtx)
		return versionResultMsg{version: version.Version, err: err}
	}
}

func (t *tui) fetchControlCmd() tea.Cmd {
	apiClient := t.client
	ctx := t.ctx
	return func() tea.Msg {
		requestCtx, cancel := context.WithTimeout(ctx, 8*time.Second)
		defer cancel()
		status, err := apiClient.GetControl(requestCtx)
		return controlResultMsg{status: status, err: err}
	}
}

func (t *tui) setOutputCmd(output string, enabled bool) tea.Cmd {
	apiClient := t.client
	ctx := t.ctx
	payload := map[string]bool{}
	if output == "MAIN" {
		payload["load_12v_on"] = enabled
	} else {
		payload["load_5v_on"] = enabled
	}

	return func() tea.Msg {
		requestCtx, cancel := context.WithTimeout(ctx, 8*time.Second)
		defer cancel()
		err := apiClient.SetControl(requestCtx, payload)
		return controlSetResultMsg{
			output:  output,
			enabled: enabled,
			err:     err,
		}
	}
}

func (t *tui) postActionCmd(action, success string) tea.Cmd {
	apiClient := t.client
	ctx := t.ctx
	return func() tea.Msg {
		requestCtx, cancel := context.WithTimeout(ctx, 8*time.Second)
		defer cancel()
		err := apiClient.SetControl(
			requestCtx,
			map[string]bool{action: true},
		)
		return actionResultMsg{
			success: success,
			failure: "Action failed",
			err:     err,
		}
	}
}

func (t *tui) fetchDiagnosticsCmd() tea.Cmd {
	if t.debugInFlight {
		return nil
	}
	t.debugInFlight = true
	apiClient := t.client
	ctx := t.ctx
	return func() tea.Msg {
		requestCtx, cancel := context.WithTimeout(ctx, 8*time.Second)
		defer cancel()
		data, err := apiClient.GetDiagnostics(requestCtx)
		return diagnosticsResultMsg{data: data, err: err}
	}
}

func (t *tui) debugTickCmd(epoch uint64) tea.Cmd {
	ctx := t.ctx
	return func() tea.Msg {
		timer := time.NewTimer(2 * time.Second)
		defer timer.Stop()
		select {
		case <-ctx.Done():
			return nil
		case <-timer.C:
			return debugTickMsg{epoch: epoch}
		}
	}
}

func (t *tui) beginUART(initialInput []byte, clearScreen bool) tea.Cmd {
	if t.uart == nil {
		t.uart = newUARTBridge(t.ctx, t.client, &t.uartLog)
		clearScreen = true
	}
	session := newUARTSession(
		t.uart,
		initialInput,
		clearScreen,
	)
	return tea.Exec(session, func(err error) tea.Msg {
		return uartExitedMsg{reason: session.reason, err: err}
	})
}

func (t *tui) stopUART() {
	if t.uart == nil {
		return
	}
	t.uart.Stop()
	t.uart = nil
}

func (t *tui) toggleRecording() {
	if t.recorder.Active() {
		filename, err := t.recorder.Stop()
		if err != nil {
			t.notice = "Stop recording failed: " + err.Error()
			return
		}
		t.notice = "CSV saved: " + filename
		return
	}

	filename := defaultRecordingFilename()
	if err := t.recorder.Start(filename); err != nil {
		t.notice = "Start recording failed: " + err.Error()
		return
	}
	t.notice = "Recording to " + filename
}

func (t *tui) View() tea.View {
	var content string
	if !t.authenticated {
		content = t.login.View(t.width, t.height)
	} else if t.confirm != nil {
		content = t.renderConfirmation()
	} else if t.activePage == pageUART {
		content = t.renderUARTMenu(t.width, t.height)
	} else {
		content = t.renderMain()
	}

	view := tea.NewView(content)
	view.AltScreen = true
	view.MouseMode = tea.MouseModeNone
	view.WindowTitle = "PMTUI - ODROID PowerMate"
	return view
}

func (t *tui) renderMain() string {
	width := max(1, t.width)
	height := max(1, t.height)
	contentHeight := max(1, height-mainReservedRows)

	var body string
	switch t.activePage {
	case pageEvents:
		body = t.renderViewportPanel("Events (latest 500)", t.eventsView, contentHeight)
	case pageDebug:
		t.updateDebugContent()
		body = t.renderViewportPanel("Runtime diagnostics", t.debugView, contentHeight)
	case pageSettings:
		body = t.renderSettings(width, contentHeight)
	default:
		body = t.renderDashboard(width, contentHeight)
	}

	return lipgloss.JoinVertical(
		lipgloss.Left,
		t.renderHeader(width),
		t.renderNavigation(width),
		body,
		t.renderFooter(width),
	)
}

func (t *tui) renderHeader(width int) string {
	wsState := lipgloss.NewStyle().
		Foreground(lipgloss.Color("196")).
		Render("OFFLINE")
	if t.wsOnline {
		wsState = lipgloss.NewStyle().
			Foreground(lipgloss.Color("82")).
			Render("ONLINE")
	}
	wifi := "Wi-Fi disconnected"
	if t.wifi.Connected {
		wifi = fmt.Sprintf(
			"%s %s %d dBm",
			t.wifi.SSID,
			t.wifi.IPAddress,
			t.wifi.RSSI,
		)
	}
	recording := ""
	if t.recorder.Active() {
		recording = "  " + lipgloss.NewStyle().
			Foreground(lipgloss.Color("196")).
			Bold(true).
			Render("REC")
	}

	text := fmt.Sprintf(
		"ODROID PowerMate  %s  ·  Firmware %s  ·  %s%s",
		wsState,
		t.version,
		wifi,
		recording,
	)
	if width < 88 {
		text = fmt.Sprintf(
			"PowerMate %s · %s · %s%s",
			t.version,
			wsState,
			wifi,
			recording,
		)
	}
	return lipgloss.NewStyle().
		Width(width).
		Align(lipgloss.Center).
		Bold(true).
		Render(text)
}

func (t *tui) renderNavigation(width int) string {
	items := []struct {
		page  page
		label string
	}{
		{pageDashboard, "1 Dashboard"},
		{pageEvents, "2 Events"},
		{pageDebug, "3 Debug"},
		{pageUART, "4 UART"},
		{pageSettings, "5 Settings"},
	}
	parts := make([]string, 0, len(items))
	for _, item := range items {
		style := lipgloss.NewStyle().Foreground(lipgloss.Color("244"))
		if item.page == t.activePage {
			style = style.
				Foreground(lipgloss.Color("232")).
				Background(lipgloss.Color("220")).
				Bold(true)
		}
		parts = append(parts, style.Render(" "+item.label+" "))
	}
	return lipgloss.NewStyle().
		Width(width).
		Align(lipgloss.Center).
		Render(strings.Join(parts, "  "))
}

func (t *tui) renderFooter(width int) string {
	text := t.notice
	if text == "" {
		switch t.activePage {
		case pageEvents:
			text = "e Clear events · ↑/↓ Scroll · r Refresh · q Quit"
		case pageDebug:
			text = "↑/↓ Scroll · r Refresh · q Quit"
		case pageSettings:
			if t.settings.baudMenuOpen {
				text = "↑↓ select · Enter choose · Esc back"
			} else {
				text = "Tab group · ↑↓ select · ←→ change · Enter edit · r reload · q quit"
			}
		default:
			text = "m MAIN · u USB · p Power · x Reset · c CSV · l Layout · r Refresh · q Quit"
		}
	}
	return lipgloss.NewStyle().
		Width(width).
		Align(lipgloss.Center).
		Foreground(lipgloss.Color("244")).
		Render(text)
}

func (t *tui) renderDashboard(width, height int) string {
	status := t.renderDashboardStatus(width)
	statusHeight := lipgloss.Height(status)
	if height-statusHeight < minHorizontalMetricGraphHeight {
		return status
	}

	graphHeight := height - statusHeight
	graphs := t.graphs.Render(
		t.lastSensor,
		width,
		graphHeight,
		t.graphLayout,
	)
	return lipgloss.JoinVertical(lipgloss.Left, graphs, status)
}

func (t *tui) renderDashboardStatus(width int) string {
	mainState := coloredStateWord(t.switches.Main)
	usbState := coloredStateWord(t.switches.USB)
	content := fmt.Sprintf(
		"Uptime %s   MAIN %s   USB %s   ",
		formatDurationMS(t.lastSensor.UptimeMS),
		mainState,
		usbState,
	)
	return lipgloss.NewStyle().
		Width(max(1, width)).
		Align(lipgloss.Center).
		Foreground(lipgloss.Color("244")).
		Render(content)
}

func (t *tui) renderViewportPanel(
	title string,
	model viewport.Model,
	height int,
) string {
	titleLine := lipgloss.NewStyle().
		Foreground(lipgloss.Color("220")).
		Bold(true).
		Render(title)
	content := lipgloss.JoinVertical(
		lipgloss.Left,
		titleLine,
		model.View(),
	)
	return lipgloss.NewStyle().
		Border(lipgloss.NormalBorder()).
		BorderForeground(lipgloss.Color("240")).
		Width(max(1, t.width)).
		Height(max(1, height)).
		MaxWidth(max(1, t.width)).
		MaxHeight(max(1, height)).
		Render(content)
}

func (t *tui) renderConfirmation() string {
	cancelStyle := lipgloss.NewStyle().
		Padding(0, 2).
		Foreground(lipgloss.Color("232")).
		Background(lipgloss.Color("220"))
	confirmStyle := lipgloss.NewStyle().Padding(0, 2)
	if t.confirm.selected {
		cancelStyle = lipgloss.NewStyle().Padding(0, 2)
		confirmStyle = confirmStyle.
			Foreground(lipgloss.Color("232")).
			Background(lipgloss.Color("196"))
	}
	buttons := lipgloss.JoinHorizontal(
		lipgloss.Top,
		cancelStyle.Render("Cancel"),
		"  ",
		confirmStyle.Render("Confirm"),
	)
	panel := lipgloss.NewStyle().
		Border(lipgloss.RoundedBorder()).
		BorderForeground(lipgloss.Color("220")).
		Padding(1, 3).
		Render(lipgloss.JoinVertical(
			lipgloss.Center,
			t.confirm.message,
			"",
			buttons,
			"",
			"←/→ select · Enter confirm · Esc cancel",
		))
	return lipgloss.Place(
		max(1, t.width),
		max(1, t.height),
		lipgloss.Center,
		lipgloss.Center,
		panel,
	)
}

func (t *tui) resizeViewports() {
	contentHeight := max(1, t.height-mainReservedRows)
	width := max(1, t.width-2)
	height := max(1, contentHeight-3)
	t.eventsView.SetWidth(width)
	t.eventsView.SetHeight(height)
	t.debugView.SetWidth(width)
	t.debugView.SetHeight(height)
}

func (t *tui) updateDebugContent() {
	if !t.haveDebug {
		t.debugView.SetContent("Diagnostics have not been received yet.")
		return
	}
	data := t.diagnostic
	wifi := "Disconnected"
	if data.WiFiConnected {
		wifi = fmt.Sprintf("Connected (%d dBm)", data.WiFiRSSI)
	}
	staState := valueOrDefault(data.WiFiSTAState, "unknown")
	lastDisconnect := "None"
	if data.WiFiLastReason != "" {
		lastDisconnect = fmt.Sprintf(
			"%s (%d)",
			data.WiFiLastReason,
			data.WiFiLastReasonCode,
		)
	}
	backoff := "Inactive"
	if strings.EqualFold(staState, "connecting") {
		if data.WiFiReconnectMS > 0 {
			backoff = fmt.Sprintf(
				"%.1f s",
				float64(data.WiFiReconnectMS)/1000,
			)
		} else {
			backoff = "Connecting now"
		}
	}
	lastConnected := "Never"
	if data.WiFiHasConnected &&
		data.UptimeSeconds >= data.WiFiLastConnected {
		lastConnected = formatDurationSeconds(
			data.UptimeSeconds-data.WiFiLastConnected,
		) + " ago"
	}
	network := strings.ToUpper(valueOrDefault(data.WiFiNetType, "unknown"))
	ipAddress := valueOrDefault(data.WiFiIPAddress, "No IP")
	lastStatus := "Never"
	if value := t.lastStatusUnixMS.Load(); value > 0 {
		lastStatus = time.Since(time.UnixMilli(value)).
			Round(time.Second).
			String() + " ago"
	}

	t.debugView.SetContent(fmt.Sprintf(
		"Uptime             %s\n"+
			"Heap               %s free / %s minimum\n"+
			"Largest block       %s\n"+
			"HTTP clients        %d\n"+
			"WebSocket           %d clients, %d/%d queued\n"+
			"UART                %s buffered, %s received\n"+
			"UART errors         FIFO %d, buffer %d\n"+
			"Queue drops         UART %d, status %d\n"+
			"WS send failures    %d\n"+
			"Wi-Fi               %s\n"+
			"STA state           %s\n"+
			"Last disconnect     %s\n"+
			"Reconnect backoff   %s\n"+
			"Last connected      %s\n"+
			"Network             %s · %s\n"+
			"Route               gateway %s · netmask %s\n"+
			"Last status message %s\n",
		formatDurationSeconds(data.UptimeSeconds),
		formatBytes(data.FreeHeapBytes),
		formatBytes(data.MinimumFreeHeap),
		formatBytes(data.LargestFreeBlock),
		data.HTTPClients,
		data.WebSocketClients,
		data.WebSocketQueueDepth,
		data.WebSocketQueueCap,
		formatBytes(data.UARTBufferedBytes),
		formatBytes(data.UARTReceivedBytes),
		data.UARTFIFOOverflows,
		data.UARTBufferFull,
		data.UARTQueueDrops,
		data.StatusQueueDrops,
		data.WebSocketFailures,
		wifi,
		staState,
		lastDisconnect,
		backoff,
		lastConnected,
		network,
		ipAddress,
		valueOrDefault(data.WiFiGateway, "-"),
		valueOrDefault(data.WiFiNetmask, "-"),
		lastStatus,
	))
}

func coloredStateWord(enabled bool) string {
	color := lipgloss.Color("196")
	word := "OFF"
	if enabled {
		color = lipgloss.Color("82")
		word = "ON"
	}
	return lipgloss.NewStyle().Foreground(color).Bold(true).Render(word)
}

func valueOrDefault(value, fallback string) string {
	if value == "" {
		return fallback
	}
	return value
}

func formatDurationMS(milliseconds uint64) string {
	return formatDurationSeconds(milliseconds / 1000)
}

func formatDurationSeconds(seconds uint64) string {
	duration := time.Duration(seconds) * time.Second
	hours := int(duration.Hours())
	minutes := int(duration.Minutes()) % 60
	secs := int(duration.Seconds()) % 60
	return fmt.Sprintf("%dh %02dm %02ds", hours, minutes, secs)
}

func formatBytes(value uint64) string {
	const (
		kib = 1024
		mib = 1024 * kib
		gib = 1024 * mib
	)
	switch {
	case value >= gib:
		return fmt.Sprintf("%.1f GiB", float64(value)/gib)
	case value >= mib:
		return fmt.Sprintf("%.1f MiB", float64(value)/mib)
	case value >= kib:
		return fmt.Sprintf("%.1f KiB", float64(value)/kib)
	default:
		return fmt.Sprintf("%d B", value)
	}
}
