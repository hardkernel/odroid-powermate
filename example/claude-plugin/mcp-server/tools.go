package main

import (
	"context"
	"errors"
	"fmt"
	"math"
	"os"
	"path/filepath"
	"regexp"
	"strconv"
	"strings"
	"time"

	"github.com/modelcontextprotocol/go-sdk/mcp"

	"github.com/hardkernel/odroid-powermate/example/claude-plugin/mcp-server/internal/console"
	"github.com/hardkernel/odroid-powermate/example/claude-plugin/mcp-server/internal/powermate"
)

const (
	defaultReadBytes = 16000
	maxReadBytes     = 200000
	// Common prompt endings: Linux shells "# " "$ ", U-Boot "=> ", GRUB "grub> ",
	// EDK2 "Shell> ", zsh "% ".
	defaultPrompt = `[#$>%] $`
	noiseWarning  = "UART output looks like line noise: the UART wire may be loose or missing, " +
		"or the SBC GND may not be shared with the PowerMate output. Do not trust this text; " +
		"ask the user to check the wiring instead of changing baud rates on the target."
)

type tools struct {
	m      *manager
	logDir string
}

// d returns the device the tools currently act on; powermate_connect can switch it.
func (t *tools) d() *device { return t.m.current() }

func boolPtr(v bool) *bool { return &v }

func registerTools(server *mcp.Server, t *tools) {
	readOnly := &mcp.ToolAnnotations{ReadOnlyHint: true, OpenWorldHint: boolPtr(false)}
	consoleWrite := &mcp.ToolAnnotations{DestructiveHint: boolPtr(true), OpenWorldHint: boolPtr(false)}
	powerAction := &mcp.ToolAnnotations{DestructiveHint: boolPtr(true), OpenWorldHint: boolPtr(false)}

	mcp.AddTool(server, &mcp.Tool{
		Name: "sbc_status",
		Description: "Snapshot of the SBC attached to the PowerMate: output switches (main = VOUT, the switched " +
			"DC input at the same 9-21 V as VIN; usb = the 5 V USB port), latest and 5 s average " +
			"voltage/current/power, stream health, console activity and " +
			"recent PowerMate events. Call this first.",
		Annotations: readOnly,
	}, t.status)

	mcp.AddTool(server, &mcp.Tool{
		Name: "sbc_power",
		Description: "Control SBC power. on/off/cycle switch an output (default main) and are a hard power " +
			"cut, like pulling the plug: shut the OS down over the console first when it is running. " +
			"press_power and press_reset pulse the open-drain Power/Reset pins of the PowerMate's J4 header, " +
			"which only do something when J4 is wired to the board's power/reset pins (ODROID-M1, M1S, M2, H4). " +
			"Returns the console " +
			"cursor from before the action so the following output can be read or waited on.",
		Annotations: powerAction,
	}, t.power)

	mcp.AddTool(server, &mcp.Tool{
		Name: "sbc_console_read",
		Description: "Read UART console output (cleaned of ANSI escapes). Without 'since' it continues " +
			"from where the last console tool stopped. When more than max_bytes is available the newest " +
			"part is returned and skipped_bytes says how much was left out.",
		Annotations: readOnly,
	}, t.consoleRead)

	mcp.AddTool(server, &mcp.Tool{
		Name: "sbc_console_send",
		Description: "Type into the SBC's UART console. 'text' is sent as-is; 'enter' appends a CR; 'keys' " +
			"sends special keys after the text (ctrl-c, ctrl-d, esc, up, down, f2, ...). For a shell " +
			"command whose output you need, prefer sbc_console_run.",
		Annotations: consoleWrite,
	}, t.consoleSend)

	mcp.AddTool(server, &mcp.Tool{
		Name: "sbc_console_wait",
		Description: "Block until a regular expression matches the console output, or until timeout. " +
			"Matching happens inside the server, so it catches output that arrives faster than a model " +
			"round trip. Searches from 'since' (default: where the last console tool stopped).",
		Annotations: readOnly,
	}, t.consoleWait)

	mcp.AddTool(server, &mcp.Tool{
		Name: "sbc_console_run",
		Description: "Run one command at an interactive prompt (Linux shell, U-Boot, GRUB, EDK2 shell) and " +
			"return its output once the prompt comes back. The console must already be at a prompt: " +
			"log in first. exit_code=true appends 'echo __PM_RC=$?' (POSIX shells only). On timeout Ctrl-C " +
			"is sent unless interrupt_on_timeout is false. If no console output has been seen since the " +
			"server started, Ctrl-C is sent first to get a fresh prompt (this would stop a foreground job).",
		Annotations: consoleWrite,
	}, t.consoleRun)

	mcp.AddTool(server, &mcp.Tool{
		Name: "sbc_boot_capture",
		Description: "Start or restart the SBC and capture its boot log until a pattern (default 'login:') " +
			"or timeout. Can stop the boot at a bootloader by sending keys when interrupt_pattern appears " +
			"(e.g. U-Boot 'Hit any key to stop autoboot'). Saves the full log to a file and returns its " +
			"path, head and tail, boot time, power summary and PowerMate events during boot.",
		Annotations: powerAction,
	}, t.bootCapture)

	mcp.AddTool(server, &mcp.Tool{
		Name: "sbc_power_record",
		Description: "Summarise power draw per channel (avg/min/max/stddev voltage, current, power, energy). " +
			"By default records for 'seconds' from now; with wait=false summarises the last 'seconds' already " +
			"buffered. Optionally writes the raw samples as CSV.",
		Annotations: readOnly,
	}, t.powerRecord)

	mcp.AddTool(server, &mcp.Tool{
		Name: "sbc_events",
		Description: "PowerMate events such as over-current cutoffs, load switch changes and limit " +
			"warnings, newest last.",
		Annotations: readOnly,
	}, t.events)
}

// ---- sbc_status ----

type statusIn struct{}

type statusOut struct {
	Host            string         `json:"host"`
	FirmwareVersion string         `json:"firmware_version,omitempty"`
	MainOn          bool           `json:"main_output_on"`
	USBOn           bool           `json:"usb_output_on"`
	Latest          map[string]any `json:"latest,omitempty"`
	Last5s          powerSummary   `json:"last_5s"`
	StatusStream    string         `json:"status_stream"`
	UARTStream      string         `json:"uart_stream"`
	UARTBaudRate    string         `json:"uart_baud_rate,omitempty"`
	ConsoleCursor   int64          `json:"console_cursor"`
	ConsoleIdleSec  *float64       `json:"console_idle_seconds,omitempty"`
	ConsoleLastLine string         `json:"console_last_line,omitempty"`
	RecentEvents    []event        `json:"recent_events,omitempty"`
	Warnings        []string       `json:"warnings,omitempty"`
}

func (t *tools) status(ctx context.Context, _ *mcp.CallToolRequest, _ statusIn) (*mcp.CallToolResult, statusOut, error) {
	out := statusOut{Host: t.d().client.Host()}
	v, err := t.d().client.GetVersion(ctx)
	if err != nil {
		// Do not report "outputs off, 0 W": nothing is known about the board.
		return nil, out, fmt.Errorf("PowerMate at %s is unreachable (%v). %s", out.Host, err, lostHint)
	}
	out.FirmwareVersion = v.Version
	if c, err := t.d().client.GetControl(ctx); err == nil {
		out.MainOn, out.USBOn = c.Main, c.USB
	}
	if s, err := t.d().client.GetSettings(ctx); err == nil {
		out.UARTBaudRate = s.BaudRate
	}

	if s, ok := t.d().latest(); ok {
		out.Latest = map[string]any{
			"age_seconds": round3(time.Since(s.At).Seconds()),
			"vin":         channelMap(s.Sensor.VIN),
			"main":        channelMap(s.Sensor.Main),
			"usb":         channelMap(s.Sensor.USB),
		}
	}
	out.Last5s = summarize(t.d().samplesSince(time.Now().Add(-5 * time.Second)))
	out.Warnings = append(out.Warnings, out.Last5s.Warnings...)

	t.d().mu.Lock()
	out.StatusStream = streamState(t.d().statusUp, t.d().statusErr)
	out.UARTStream = streamState(t.d().uartUp, t.d().uartErr)
	t.d().mu.Unlock()

	out.ConsoleCursor = t.d().console.End()
	if last := t.d().console.LastWrite(); !last.IsZero() {
		idle := round3(time.Since(last).Seconds())
		out.ConsoleIdleSec = &idle
		snap := t.d().console.Since(out.ConsoleCursor - 4096)
		text := console.Clean(snap.Data)
		out.ConsoleLastLine = lastLine(text.String)
		if text.LooksLikeNoise() {
			out.Warnings = append(out.Warnings, noiseWarning)
		}
	}
	out.RecentEvents = t.d().eventsSince(time.Now().Add(-10 * time.Minute))
	if n := len(out.RecentEvents); n > 10 {
		out.RecentEvents = out.RecentEvents[n-10:]
	}
	return nil, out, nil
}

// ---- sbc_power ----

type powerIn struct {
	Action     string  `json:"action" jsonschema:"one of: on, off, cycle, press_power, press_reset"`
	Output     string  `json:"output,omitempty" jsonschema:"for on/off/cycle: main (VOUT DC output, default), usb (5 V USB port) or both"`
	OffSeconds float64 `json:"off_seconds,omitempty" jsonschema:"for cycle: seconds to stay off (default 3)"`
}

type powerOut struct {
	Action       string `json:"action"`
	MainOn       bool   `json:"main_output_on"`
	USBOn        bool   `json:"usb_output_on"`
	CursorBefore int64  `json:"console_cursor_before"`
	Note         string `json:"note,omitempty"`
}

func (t *tools) power(ctx context.Context, _ *mcp.CallToolRequest, in powerIn) (*mcp.CallToolResult, powerOut, error) {
	out := powerOut{Action: in.Action, CursorBefore: t.d().console.End()}
	keys, err := outputKeys(in.Output)
	if err != nil {
		return nil, out, err
	}
	set := func(on bool) error {
		payload := map[string]bool{}
		for _, k := range keys {
			payload[k] = on
		}
		return t.d().client.SetControl(ctx, payload)
	}

	switch in.Action {
	case "on":
		err = set(true)
	case "off":
		err = set(false)
	case "cycle":
		off := in.OffSeconds
		if off <= 0 {
			off = 3
		}
		if off > 60 {
			return nil, out, errors.New("off_seconds must be 60 or less")
		}
		if err = set(false); err == nil {
			select {
			case <-time.After(time.Duration(off * float64(time.Second))):
			case <-ctx.Done():
				return nil, out, ctx.Err()
			}
			out.CursorBefore = t.d().console.End()
			err = set(true)
		}
	case "press_power":
		err = t.d().client.SetControl(ctx, map[string]bool{"power_trigger": true})
		out.Note = "power button line is held for the firmware's trigger time (3 s by default)"
	case "press_reset":
		err = t.d().client.SetControl(ctx, map[string]bool{"reset_trigger": true})
		out.Note = "reset line is held for the firmware's trigger time (1 s by default)"
	default:
		return nil, out, fmt.Errorf("unknown action %q", in.Action)
	}
	if err != nil {
		return nil, out, err
	}
	if c, err := t.d().client.GetControl(ctx); err == nil {
		out.MainOn, out.USBOn = c.Main, c.USB
	}
	return nil, out, nil
}

func outputKeys(output string) ([]string, error) {
	switch output {
	case "", "main":
		return []string{"load_12v_on"}, nil
	case "usb":
		return []string{"load_5v_on"}, nil
	case "both":
		return []string{"load_12v_on", "load_5v_on"}, nil
	}
	return nil, fmt.Errorf("unknown output %q (want main, usb or both)", output)
}

// ---- sbc_console_read ----

type readIn struct {
	Since    *int64 `json:"since,omitempty" jsonschema:"console cursor to read from; default continues after the last console tool"`
	MaxBytes int    `json:"max_bytes,omitempty" jsonschema:"maximum characters to return (default 16000)"`
	Grep     string `json:"grep,omitempty" jsonschema:"optional regular expression; only matching lines are returned"`
}

type readOut struct {
	Text         string   `json:"text"`
	From         int64    `json:"from_cursor"`
	Cursor       int64    `json:"cursor"`
	SkippedBytes int      `json:"skipped_bytes,omitempty"`
	LostBytes    int64    `json:"lost_bytes,omitempty"`
	Warnings     []string `json:"warnings,omitempty"`
}

func (t *tools) consoleRead(_ context.Context, _ *mcp.CallToolRequest, in readIn) (*mcp.CallToolResult, readOut, error) {
	since := t.d().getReadCursor()
	if in.Since != nil {
		since = *in.Since
	}
	limit := clampInt(in.MaxBytes, defaultReadBytes, maxReadBytes)
	snap := t.d().console.Since(since)
	text := console.Clean(snap.Data)
	out := readOut{From: snap.From, Cursor: snap.End, LostBytes: snap.Lost}

	body := text.String
	if in.Grep != "" {
		re, err := regexp.Compile(in.Grep)
		if err != nil {
			return nil, out, fmt.Errorf("grep: %w", err)
		}
		var lines []string
		for _, line := range strings.Split(body, "\n") {
			if re.MatchString(line) {
				lines = append(lines, line)
			}
		}
		body = strings.Join(lines, "\n")
	}
	out.Text, out.SkippedBytes = tail(body, limit)
	if text.LooksLikeNoise() {
		out.Warnings = append(out.Warnings, noiseWarning)
	}
	if snap.Lost > 0 {
		out.Warnings = append(out.Warnings, fmt.Sprintf("%d bytes before this point were dropped from the 4 MiB buffer", snap.Lost))
	}
	t.d().setReadCursor(snap.End)
	return nil, out, nil
}

// ---- sbc_console_send ----

type sendIn struct {
	Text  string   `json:"text,omitempty" jsonschema:"text to type, sent as-is"`
	Enter bool     `json:"enter,omitempty" jsonschema:"append a carriage return after text"`
	Keys  []string `json:"keys,omitempty" jsonschema:"special keys sent after text: enter, ctrl-c, ctrl-d, ctrl-z, ctrl-x, esc, tab, space, backspace, up, down, left, right, home, end, delete, pgup, pgdn, f1..f12"`
}

type sendOut struct {
	BytesSent    int   `json:"bytes_sent"`
	CursorBefore int64 `json:"console_cursor_before"`
}

var keyCodes = map[string]string{
	"enter": "\r", "ctrl-c": "\x03", "ctrl-d": "\x04", "ctrl-z": "\x1a", "ctrl-x": "\x18",
	"ctrl-a": "\x01", "ctrl-e": "\x05", "ctrl-u": "\x15", "ctrl-l": "\x0c",
	"esc": "\x1b", "tab": "\t", "space": " ", "backspace": "\x7f",
	"up": "\x1b[A", "down": "\x1b[B", "right": "\x1b[C", "left": "\x1b[D",
	"home": "\x1b[H", "end": "\x1b[F", "delete": "\x1b[3~", "pgup": "\x1b[5~", "pgdn": "\x1b[6~",
	"f1": "\x1bOP", "f2": "\x1bOQ", "f3": "\x1bOR", "f4": "\x1bOS",
	"f5": "\x1b[15~", "f6": "\x1b[17~", "f7": "\x1b[18~", "f8": "\x1b[19~",
	"f9": "\x1b[20~", "f10": "\x1b[21~", "f11": "\x1b[23~", "f12": "\x1b[24~",
}

func (t *tools) consoleSend(ctx context.Context, _ *mcp.CallToolRequest, in sendIn) (*mcp.CallToolResult, sendOut, error) {
	payload := in.Text
	if in.Enter {
		payload += "\r"
	}
	for _, k := range in.Keys {
		code, ok := keyCodes[strings.ToLower(k)]
		if !ok {
			return nil, sendOut{}, fmt.Errorf("unknown key %q", k)
		}
		payload += code
	}
	if payload == "" {
		return nil, sendOut{}, errors.New("nothing to send: give text, enter or keys")
	}
	out := sendOut{CursorBefore: t.d().console.End()}
	if err := t.d().send(ctx, []byte(payload)); err != nil {
		return nil, out, err
	}
	out.BytesSent = len(payload)
	t.d().setReadCursor(out.CursorBefore)
	return nil, out, nil
}

// ---- sbc_console_wait ----

type waitIn struct {
	Pattern        string `json:"pattern" jsonschema:"regular expression (Go RE2 syntax) matched against cleaned console text"`
	TimeoutSeconds int    `json:"timeout_seconds,omitempty" jsonschema:"default 30, max 900"`
	Since          *int64 `json:"since,omitempty" jsonschema:"console cursor to search from; default continues after the last console tool"`
}

type waitOut struct {
	Matched  bool     `json:"matched"`
	Match    string   `json:"match,omitempty"`
	Context  string   `json:"context"`
	Cursor   int64    `json:"cursor"`
	Elapsed  float64  `json:"elapsed_seconds"`
	Warnings []string `json:"warnings,omitempty"`
}

func (t *tools) consoleWait(ctx context.Context, _ *mcp.CallToolRequest, in waitIn) (*mcp.CallToolResult, waitOut, error) {
	re, err := regexp.Compile(in.Pattern)
	if err != nil {
		return nil, waitOut{}, fmt.Errorf("pattern: %w", err)
	}
	since := t.d().getReadCursor()
	if in.Since != nil {
		since = *in.Since
	}
	timeout := time.Duration(clampInt(in.TimeoutSeconds, 30, 900)) * time.Second
	start := time.Now()
	m := waitFor(ctx, t.d().console, since, timeout, re)
	out := waitOut{Matched: m.ok, Match: m.match, Cursor: m.cursor, Elapsed: round3(time.Since(start).Seconds())}
	out.Context, _ = tail(m.text.String[:m.textEnd], 4000)
	if m.text.LooksLikeNoise() {
		out.Warnings = append(out.Warnings, noiseWarning)
	}
	t.d().setReadCursor(m.cursor)
	return nil, out, nil
}

type matchResult struct {
	ok      bool
	match   string
	cursor  int64 // just past the match, or the end of data on timeout
	text    console.Text
	textEnd int // index in text.String just past the match
}

func waitFor(ctx context.Context, buf *console.Buffer, since int64, timeout time.Duration, re *regexp.Regexp) matchResult {
	var res matchResult
	buf.Wait(ctx, since, timeout, func(s console.Snapshot) bool {
		res.text = console.Clean(s.Data)
		res.cursor = s.End
		res.textEnd = len(res.text.String)
		loc := re.FindStringIndex(res.text.String)
		if loc == nil {
			return false
		}
		res.ok = true
		res.match = res.text.String[loc[0]:loc[1]]
		res.textEnd = loc[1]
		res.cursor = s.From + int64(res.text.RawOffset(loc[1]))
		return true
	})
	return res
}

// ---- sbc_console_run ----

type runIn struct {
	Command            string `json:"command" jsonschema:"command line to type; a CR is appended"`
	Prompt             string `json:"prompt,omitempty" jsonschema:"regular expression for the prompt at the end of the output (default '[#$>%] $')"`
	TimeoutSeconds     int    `json:"timeout_seconds,omitempty" jsonschema:"default 30, max 900"`
	ExitCode           bool   `json:"exit_code,omitempty" jsonschema:"POSIX shells only: also report the command's exit status"`
	InterruptOnTimeout *bool  `json:"interrupt_on_timeout,omitempty" jsonschema:"send Ctrl-C if the prompt does not return in time (default true)"`
}

type runOut struct {
	Output     string   `json:"output"`
	ExitCode   *int     `json:"exit_code,omitempty"`
	PromptSeen bool     `json:"prompt_seen"`
	TimedOut   bool     `json:"timed_out"`
	Cursor     int64    `json:"cursor"`
	Elapsed    float64  `json:"elapsed_seconds"`
	Truncated  int      `json:"truncated_bytes,omitempty"`
	Warnings   []string `json:"warnings,omitempty"`
}

var exitCodeLine = regexp.MustCompile(`(?m)^__PM_RC=(\d+)\s*$`)

var loginPrompt = regexp.MustCompile(`(?i)(login|password|username)[^\n]{0,20}: ?\z`)

func (t *tools) consoleRun(ctx context.Context, _ *mcp.CallToolRequest, in runIn) (*mcp.CallToolResult, runOut, error) {
	if strings.ContainsAny(in.Command, "\r\n") {
		return nil, runOut{}, errors.New("command must be a single line")
	}
	prompt := in.Prompt
	if prompt == "" {
		prompt = defaultPrompt
	}
	// The prompt must be the last thing printed, not just appear somewhere.
	promptRe, err := regexp.Compile(`(?:` + prompt + `)\z`)
	if err != nil {
		return nil, runOut{}, fmt.Errorf("prompt: %w", err)
	}
	timeout := time.Duration(clampInt(in.TimeoutSeconds, 30, 900)) * time.Second

	line := in.Command
	if in.ExitCode {
		line += `; echo "__PM_RC=$?"`
	}
	if err := t.d().waitUART(ctx, 5*time.Second); err != nil {
		return nil, runOut{}, err
	}
	// Let earlier output (an unfinished boot message, a previous prompt) settle so it
	// is not mistaken for this command's prompt.
	var warnings []string
	if t.d().console.End() == 0 {
		// Nothing received since this server started, so the console state is unknown.
		// Ctrl-C is harmless at shell, U-Boot, GRUB and login prompts and makes the
		// target print a fresh prompt to inspect.
		if err := t.d().send(ctx, []byte("\x03")); err != nil {
			return nil, runOut{}, err
		}
		t.d().console.Wait(ctx, 0, 1500*time.Millisecond, func(s console.Snapshot) bool { return len(s.Data) > 0 })
		warnings = append(warnings, "console state was unknown; sent Ctrl-C first to get a fresh prompt")
	}
	t.d().console.WaitQuiet(ctx, 200*time.Millisecond, 2*time.Second)
	cursor := t.d().console.End()
	// Typing a command at a login prompt would submit it as a user name, and typing
	// into line noise means the console state is unknown.
	recentText := console.Clean(t.d().console.Since(cursor - 512).Data)
	if recent := recentText.String; loginPrompt.MatchString(recent) {
		return nil, runOut{}, fmt.Errorf("console is at a login prompt (%q): log in with sbc_console_send first",
			lastLine(recent))
	}
	if recentText.LooksLikeNoise() {
		return nil, runOut{}, errors.New("the recent console output is line noise, so the console state is unknown " +
			"and the command was not sent. " + noiseWarning)
	}
	start := time.Now()
	if err := t.d().send(ctx, []byte(line+"\r")); err != nil {
		return nil, runOut{}, err
	}

	var text console.Text
	var end int64
	promptSeen := false
	deadline := time.Now().Add(timeout)
	for !promptSeen && time.Now().Before(deadline) {
		ok, snap := t.d().console.Wait(ctx, cursor, time.Until(deadline), func(s console.Snapshot) bool {
			text = console.Clean(s.Data)
			return promptRe.MatchString(text.String)
		})
		end = snap.End
		if !ok {
			break
		}
		// A prompt-like ending can be part of the output; accept it only if nothing
		// follows for a short while.
		t.d().console.WaitQuiet(ctx, 300*time.Millisecond, 3*time.Second)
		if t.d().console.End() == snap.End {
			promptSeen = true
		}
	}
	if ctx.Err() != nil {
		return nil, runOut{}, ctx.Err()
	}

	out := runOut{PromptSeen: promptSeen, TimedOut: !promptSeen, Cursor: end, Elapsed: round3(time.Since(start).Seconds()),
		Warnings: warnings}
	if end == cursor {
		out.Warnings = append(out.Warnings, "the SBC sent nothing back, not even the echo of the command: it may not "+
			"receive PowerMate TX (check the UART RX wire on the SBC side), its console may be on another UART, "+
			"or it is hung")
	}
	if !promptSeen && (in.InterruptOnTimeout == nil || *in.InterruptOnTimeout) {
		_ = t.d().send(ctx, []byte("\x03"))
		out.Warnings = append(out.Warnings, "prompt did not return in time; sent Ctrl-C")
	}

	body := stripEcho(text.String, line)
	if promptSeen {
		body = stripPrompt(body)
	}
	if in.ExitCode {
		if m := exitCodeLine.FindAllStringSubmatchIndex(body, -1); len(m) > 0 {
			last := m[len(m)-1]
			if code, err := strconv.Atoi(body[last[2]:last[3]]); err == nil {
				out.ExitCode = &code
			}
			body = body[:last[0]] + body[last[1]:]
		}
	}
	out.Output, out.Truncated = tail(strings.TrimRight(body, "\n"), defaultReadBytes)
	if text.LooksLikeNoise() {
		out.Warnings = append(out.Warnings, noiseWarning)
	}
	t.d().setReadCursor(end)
	return nil, out, nil
}

// stripEcho drops the terminal's echo of the command line.
func stripEcho(text, line string) string {
	first, rest, found := strings.Cut(text, "\n")
	if found && strings.Contains(first, strings.TrimSpace(line)) {
		return rest
	}
	// Long command lines wrap; fall back to dropping everything up to the last
	// occurrence of the command's tail.
	if i := strings.Index(text, lastN(line, 20)+"\n"); i >= 0 {
		return text[i+len(lastN(line, 20))+1:]
	}
	return text
}

// stripPrompt removes the final prompt line.
func stripPrompt(text string) string {
	if i := strings.LastIndex(text, "\n"); i >= 0 {
		return text[:i+1]
	}
	return ""
}

// ---- sbc_boot_capture ----

type bootIn struct {
	Method           string   `json:"method,omitempty" jsonschema:"power_cycle (default), power_on, reset_button, power_button or none (only capture)"`
	Until            string   `json:"until,omitempty" jsonschema:"regular expression that ends the capture (default 'login:')"`
	TimeoutSeconds   int      `json:"timeout_seconds,omitempty" jsonschema:"default 180, max 900"`
	InterruptPattern string   `json:"interrupt_pattern,omitempty" jsonschema:"regular expression that triggers sending interrupt_keys, e.g. 'Hit any key to stop autoboot'"`
	InterruptKeys    []string `json:"interrupt_keys,omitempty" jsonschema:"key names as in sbc_console_send (ctrl-c, esc, f2, space, ...) or literal text; default [space]"`
	InterruptRepeat  float64  `json:"interrupt_repeat_seconds,omitempty" jsonschema:"keep resending interrupt_keys every 20 ms for this many seconds after interrupt_pattern (until 'until' matches); needed for bootloaders with a zero boot delay"`
	OffSeconds       float64  `json:"off_seconds,omitempty" jsonschema:"for power_cycle: seconds to stay off (default 3)"`
}

type bootOut struct {
	Matched        bool         `json:"matched"`
	Match          string       `json:"match,omitempty"`
	BootSeconds    float64      `json:"boot_seconds"`
	Interrupted    bool         `json:"interrupted"`
	LogPath        string       `json:"log_path"`
	RawLogPath     string       `json:"raw_log_path"`
	Lines          int          `json:"lines"`
	Head           string       `json:"head"`
	Tail           string       `json:"tail"`
	Cursor         int64        `json:"cursor"`
	Power          powerSummary `json:"power_during_boot"`
	PowerMateEvent []event      `json:"powermate_events,omitempty"`
	Warnings       []string     `json:"warnings,omitempty"`
}

func (t *tools) bootCapture(ctx context.Context, _ *mcp.CallToolRequest, in bootIn) (*mcp.CallToolResult, bootOut, error) {
	until := in.Until
	if until == "" {
		until = `login:`
	}
	untilRe, err := regexp.Compile(until)
	if err != nil {
		return nil, bootOut{}, fmt.Errorf("until: %w", err)
	}
	var interruptRe *regexp.Regexp
	if in.InterruptPattern != "" {
		if interruptRe, err = regexp.Compile(in.InterruptPattern); err != nil {
			return nil, bootOut{}, fmt.Errorf("interrupt_pattern: %w", err)
		}
	}
	keys := " "
	if len(in.InterruptKeys) > 0 {
		keys = keySequence(in.InterruptKeys)
	}
	if in.InterruptRepeat < 0 || in.InterruptRepeat > 60 {
		return nil, bootOut{}, errors.New("interrupt_repeat_seconds must be between 0 and 60")
	}
	timeout := time.Duration(clampInt(in.TimeoutSeconds, 180, 900)) * time.Second

	// Connect the UART before touching power so the first bootloader lines are kept.
	if err := t.d().waitUART(ctx, 5*time.Second); err != nil {
		return nil, bootOut{}, err
	}

	method := in.Method
	if method == "" {
		method = "power_cycle"
	}
	var start time.Time
	var cursor int64
	mark := func() { start, cursor = time.Now(), t.d().console.End() }
	switch method {
	case "power_cycle":
		off := in.OffSeconds
		if off <= 0 {
			off = 3
		}
		if err := t.d().client.SetControl(ctx, map[string]bool{"load_12v_on": false}); err != nil {
			return nil, bootOut{}, err
		}
		select {
		case <-time.After(time.Duration(off * float64(time.Second))):
		case <-ctx.Done():
			return nil, bootOut{}, ctx.Err()
		}
		mark()
		err = t.d().client.SetControl(ctx, map[string]bool{"load_12v_on": true})
	case "power_on":
		mark()
		err = t.d().client.SetControl(ctx, map[string]bool{"load_12v_on": true})
	case "reset_button":
		mark()
		err = t.d().client.SetControl(ctx, map[string]bool{"reset_trigger": true})
	case "power_button":
		mark()
		err = t.d().client.SetControl(ctx, map[string]bool{"power_trigger": true})
	case "none":
		mark()
	default:
		return nil, bootOut{}, fmt.Errorf("unknown method %q", method)
	}
	if err != nil {
		return nil, bootOut{}, err
	}

	out := bootOut{}
	interruptFrom := 0
	m := matchResult{}
	stopRepeat := make(chan struct{})
	defer close(stopRepeat)
	ok, snap := t.d().console.Wait(ctx, cursor, timeout, func(s console.Snapshot) bool {
		text := console.Clean(s.Data)
		if interruptRe != nil && !out.Interrupted {
			if loc := interruptRe.FindStringIndex(text.String); loc != nil {
				// Sent from inside the wait so the bootloader's countdown is not missed.
				if t.d().client.SendUART([]byte(keys)) == nil {
					out.Interrupted = true
					interruptFrom = loc[1]
					if in.InterruptRepeat > 0 {
						go t.repeatKeys(keys, time.Duration(in.InterruptRepeat*float64(time.Second)), stopRepeat)
					}
				}
			}
		}
		search := text.String[min(interruptFrom, len(text.String)):]
		if loc := untilRe.FindStringIndex(search); loc != nil {
			m = matchResult{ok: true, match: search[loc[0]:loc[1]], text: text}
			m.cursor = s.From + int64(text.RawOffset(interruptFrom+loc[1]))
			return true
		}
		return false
	})
	if ctx.Err() != nil {
		return nil, bootOut{}, ctx.Err()
	}
	out.Matched = ok
	out.Match = m.match
	if interruptRe != nil && !out.Interrupted {
		out.Warnings = append(out.Warnings, "interrupt_pattern never appeared, so no keys were sent and the boot "+
			"was not stopped (line noise can corrupt the banner it waits for)")
	}
	out.BootSeconds = round3(time.Since(start).Seconds())
	out.Cursor = snap.End
	if ok {
		out.Cursor = m.cursor
	}

	text := console.Clean(snap.Data)
	stamp := start.Format("20060102-150405")
	out.LogPath = filepath.Join(t.logDir, "boot-"+stamp+".log")
	out.RawLogPath = filepath.Join(t.logDir, "boot-"+stamp+".raw")
	if err := os.MkdirAll(t.logDir, 0o755); err != nil {
		return nil, out, err
	}
	if err := os.WriteFile(out.LogPath, []byte(text.String), 0o644); err != nil {
		return nil, out, err
	}
	if err := os.WriteFile(out.RawLogPath, snap.Data, 0o644); err != nil {
		return nil, out, err
	}

	lines := strings.Split(strings.TrimRight(text.String, "\n"), "\n")
	out.Lines = len(lines)
	out.Head = strings.Join(lines[:min(40, len(lines))], "\n")
	out.Tail = strings.Join(lines[max(len(lines)-60, min(40, len(lines))):], "\n")
	out.Power = summarize(t.d().samplesSince(start))
	out.PowerMateEvent = t.d().eventsSince(start.Add(-time.Second))
	out.Warnings = append(out.Warnings, out.Power.Warnings...)
	if text.LooksLikeNoise() {
		out.Warnings = append(out.Warnings, noiseWarning)
	}
	if snap.Lost > 0 {
		out.Warnings = append(out.Warnings, "boot output exceeded the console buffer; the start of the log is missing")
	}
	if len(snap.Data) == 0 {
		out.Warnings = append(out.Warnings,
			"no UART output at all: the SBC may not be powered, its console may be on another UART, or the UART wire is disconnected")
	}
	t.d().setReadCursor(out.Cursor)
	return nil, out, nil
}

// repeatKeys resends keys until stop is closed or d passes. Some bootloaders (e.g.
// Rockchip U-Boot with bootdelay=0) only poll the UART for an instant.
func (t *tools) repeatKeys(keys string, d time.Duration, stop <-chan struct{}) {
	ticker := time.NewTicker(20 * time.Millisecond)
	defer ticker.Stop()
	deadline := time.After(d)
	for {
		select {
		case <-stop:
			return
		case <-deadline:
			return
		case <-ticker.C:
			_ = t.d().client.SendUART([]byte(keys))
		}
	}
}

// keySequence turns key names into their codes; other entries are sent literally.
func keySequence(keys []string) string {
	var b strings.Builder
	for _, k := range keys {
		if code, ok := keyCodes[strings.ToLower(k)]; ok {
			b.WriteString(code)
		} else {
			b.WriteString(k)
		}
	}
	return b.String()
}

// ---- sbc_power_record ----

type recordIn struct {
	Seconds float64 `json:"seconds" jsonschema:"window length in seconds (1 to 600)"`
	Wait    *bool   `json:"wait,omitempty" jsonschema:"true (default): record from now; false: summarise the last 'seconds' already buffered"`
	CSV     bool    `json:"csv,omitempty" jsonschema:"also write the samples to a CSV file and return its path"`
}

type recordOut struct {
	powerSummary
	CSVPath string `json:"csv_path,omitempty"`
}

func (t *tools) powerRecord(ctx context.Context, _ *mcp.CallToolRequest, in recordIn) (*mcp.CallToolResult, recordOut, error) {
	if in.Seconds < 1 || in.Seconds > 600 {
		return nil, recordOut{}, errors.New("seconds must be between 1 and 600")
	}
	window := time.Duration(in.Seconds * float64(time.Second))
	from := time.Now().Add(-window)
	if in.Wait == nil || *in.Wait {
		from = time.Now()
		select {
		case <-time.After(window):
		case <-ctx.Done():
			return nil, recordOut{}, ctx.Err()
		}
	}
	samples := t.d().samplesSince(from)
	out := recordOut{powerSummary: summarize(samples)}
	if len(samples) == 0 {
		out.Warnings = append(out.Warnings, "no sensor samples received in this window: is the status stream connected?")
	}
	if in.CSV && len(samples) > 0 {
		var b strings.Builder
		b.WriteString("host_time,device_time_ms,vin_v,vin_a,vin_w,main_v,main_a,main_w,usb_v,usb_a,usb_w\n")
		for _, s := range samples {
			fmt.Fprintf(&b, "%s,%d,%.3f,%.4f,%.3f,%.3f,%.4f,%.3f,%.3f,%.4f,%.3f\n",
				s.At.Format(time.RFC3339Nano), s.Sensor.TimestampMS,
				s.Sensor.VIN.Voltage, s.Sensor.VIN.Current, s.Sensor.VIN.Power,
				s.Sensor.Main.Voltage, s.Sensor.Main.Current, s.Sensor.Main.Power,
				s.Sensor.USB.Voltage, s.Sensor.USB.Current, s.Sensor.USB.Power)
		}
		if err := os.MkdirAll(t.logDir, 0o755); err != nil {
			return nil, out, err
		}
		out.CSVPath = filepath.Join(t.logDir, "power-"+time.Now().Format("20060102-150405")+".csv")
		if err := os.WriteFile(out.CSVPath, []byte(b.String()), 0o644); err != nil {
			return nil, out, err
		}
	}
	return nil, out, nil
}

// ---- sbc_events ----

type eventsIn struct {
	Limit int `json:"limit,omitempty" jsonschema:"maximum number of events (default 50)"`
}

type eventsOut struct {
	Events []event `json:"events"`
}

func (t *tools) events(_ context.Context, _ *mcp.CallToolRequest, in eventsIn) (*mcp.CallToolResult, eventsOut, error) {
	all := t.d().eventsSince(time.Time{})
	limit := clampInt(in.Limit, 50, maxEvents)
	if len(all) > limit {
		all = all[len(all)-limit:]
	}
	if all == nil {
		all = []event{}
	}
	return nil, eventsOut{Events: all}, nil
}

// ---- helpers ----

func channelMap(ch powermate.ChannelData) map[string]float64 {
	return map[string]float64{
		"voltage_v": round3(float64(ch.Voltage)),
		"current_a": round3(float64(ch.Current)),
		"power_w":   round3(float64(ch.Power)),
	}
}

func streamState(up bool, lastErr string) string {
	if up {
		return "connected"
	}
	if lastErr != "" {
		return "disconnected: " + lastErr
	}
	return "connecting"
}

func clampInt(v, def, max int) int {
	if v <= 0 {
		return def
	}
	if v > max {
		return max
	}
	return v
}

// tail keeps the last limit bytes of s on a line boundary and reports how many
// bytes were dropped.
func tail(s string, limit int) (string, int) {
	if len(s) <= limit {
		return s, 0
	}
	cut := len(s) - limit
	if s[cut-1] != '\n' {
		if i := strings.IndexByte(s[cut:], '\n'); i >= 0 && i < limit/4 {
			cut += i + 1
		}
	}
	return s[cut:], cut
}

func lastLine(s string) string {
	s = strings.TrimRight(s, "\n")
	if i := strings.LastIndex(s, "\n"); i >= 0 {
		s = s[i+1:]
	}
	if len(s) > 200 {
		s = s[len(s)-200:]
	}
	return s
}

func lastN(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[len(s)-n:]
}

func round3(v float64) float64 {
	return math.Round(v*1000) / 1000
}
