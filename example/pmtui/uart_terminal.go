package main

import (
	"context"
	"errors"
	"fmt"
	"io"
	"os"
	"sync"
	"sync/atomic"
	"time"

	uv "github.com/charmbracelet/ultraviolet"
	"github.com/charmbracelet/x/ansi"
	"github.com/charmbracelet/x/vt"
	"golang.org/x/term"
)

const (
	maxUARTBytes        = 256 * 1024
	maxUARTPendingBytes = 1024 * 1024
	maxUARTTXMessages   = 128
	uartMenuByte        = byte(0x14)
	clearUARTTerminal   = ansi.EraseEntireScreen + ansi.EraseEntireDisplay +
		ansi.CursorHomePosition
)

type uartExitReason uint8

const (
	uartExitMenu uartExitReason = iota
	uartExitEOF
	uartExitError
)

type uartLog struct {
	mu   sync.Mutex
	data []byte
}

func (log *uartLog) Write(data []byte) (int, error) {
	log.mu.Lock()
	defer log.mu.Unlock()

	count := len(data)
	if count >= maxUARTBytes {
		log.data = append(log.data[:0], data[count-maxUARTBytes:]...)
		return count, nil
	}

	log.data = append(log.data, data...)
	if len(log.data) > maxUARTBytes {
		drop := len(log.data) - maxUARTBytes
		copy(log.data, log.data[drop:])
		log.data = log.data[:maxUARTBytes]
	}
	return count, nil
}

func (log *uartLog) Snapshot() []byte {
	log.mu.Lock()
	defer log.mu.Unlock()
	return append([]byte(nil), log.data...)
}

func (log *uartLog) Reset() {
	log.mu.Lock()
	log.data = log.data[:0]
	log.mu.Unlock()
}

type uartWriteRequest struct {
	data []byte
	done chan error
}

type uartBridge struct {
	ctx    context.Context
	cancel context.CancelFunc
	client *client
	log    *uartLog

	tx        chan uartWriteRequest
	connected atomic.Bool

	outputMu   sync.Mutex
	output     io.Writer
	pending    []byte
	shadow     *vt.SafeEmulator
	shadowDone chan struct{}

	modesMu sync.Mutex
	modes   map[ansi.Mode]bool

	workers sync.WaitGroup
}

func newUARTBridge(
	parent context.Context,
	apiClient *client,
	log *uartLog,
) *uartBridge {
	ctx, cancel := context.WithCancel(parent)
	bridge := &uartBridge{
		ctx:        ctx,
		cancel:     cancel,
		client:     apiClient,
		log:        log,
		tx:         make(chan uartWriteRequest, maxUARTTXMessages),
		shadow:     vt.NewSafeEmulator(80, 24),
		shadowDone: make(chan struct{}),
		modes:      make(map[ansi.Mode]bool),
	}
	bridge.shadow.SetCallbacks(vt.Callbacks{
		EnableMode: func(mode ansi.Mode) {
			bridge.modesMu.Lock()
			bridge.modes[mode] = true
			bridge.modesMu.Unlock()
		},
		DisableMode: func(mode ansi.Mode) {
			bridge.modesMu.Lock()
			bridge.modes[mode] = false
			bridge.modesMu.Unlock()
		},
	})

	bridge.workers.Add(2)
	go bridge.drainShadowResponses()
	go func() {
		defer bridge.workers.Done()
		bridge.client.RunUART(
			ctx,
			bridge.receive,
			func(connected bool, _ error) {
				bridge.connected.Store(connected)
			},
		)
	}()
	go func() {
		defer bridge.workers.Done()
		bridge.sendLoop()
	}()
	return bridge
}

func (bridge *uartBridge) drainShadowResponses() {
	defer close(bridge.shadowDone)
	buffer := make([]byte, 4096)
	for {
		count, err := bridge.shadow.Read(buffer)
		if count > 0 {
			bridge.outputMu.Lock()
			attached := bridge.output != nil
			bridge.outputMu.Unlock()
			if !attached {
				_ = bridge.Queue(buffer[:count])
			}
		}
		if err != nil {
			return
		}
	}
}

func (bridge *uartBridge) receive(data []byte) {
	bridge.outputMu.Lock()
	_, _ = bridge.log.Write(data)
	if bridge.output != nil {
		_, _ = bridge.output.Write(data)
	} else {
		bridge.pending = append(bridge.pending, data...)
		if len(bridge.pending) > maxUARTPendingBytes {
			drop := len(bridge.pending) - maxUARTPendingBytes
			copy(bridge.pending, bridge.pending[drop:])
			bridge.pending = bridge.pending[:maxUARTPendingBytes]
		}
	}
	bridge.outputMu.Unlock()

	// x/vt can emit multiple terminal responses while parsing one input
	// buffer. Do not hold outputMu here: the response drain also needs that
	// mutex to decide whether the host terminal is currently attached.
	_, _ = bridge.shadow.Write(data)
}

func (bridge *uartBridge) Attach(
	output io.Writer,
	width int,
	height int,
	clearScreen bool,
) {
	width = max(1, width)
	height = max(1, height)
	bridge.shadow.Resize(width, height)

	bridge.outputMu.Lock()
	defer bridge.outputMu.Unlock()

	switch {
	case clearScreen:
		_, _ = io.WriteString(output, clearUARTTerminal)
	case bridge.shadow.IsAltScreen():
		_, _ = io.WriteString(output, ansi.SetMode(ansi.ModeAltScreenSaveCursor))
		_, _ = io.WriteString(output, ansi.EraseEntireScreen+ansi.CursorHomePosition)
		_, _ = io.WriteString(output, bridge.shadow.Render())
		cursor := bridge.shadow.CursorPosition()
		_, _ = io.WriteString(
			output,
			ansi.CursorPosition(cursor.X+1, cursor.Y+1),
		)
	default:
		_, _ = output.Write(bridge.pending)
	}

	bridge.restoreModes(output)
	bridge.pending = bridge.pending[:0]
	bridge.output = output
}

func (bridge *uartBridge) Detach() {
	bridge.outputMu.Lock()
	bridge.output = nil
	bridge.outputMu.Unlock()
}

func (bridge *uartBridge) restoreModes(output io.Writer) {
	bridge.modesMu.Lock()
	defer bridge.modesMu.Unlock()

	for mode, enabled := range bridge.modes {
		switch mode {
		case ansi.ModeAltScreen,
			ansi.ModeAltScreenSaveCursor,
			ansi.ModeSynchronizedOutput:
			continue
		}
		sequence := ansi.ResetMode(mode)
		if enabled {
			sequence = ansi.SetMode(mode)
		}
		_, _ = io.WriteString(output, sequence)
	}
}

func (bridge *uartBridge) Queue(data []byte) error {
	if len(data) == 0 {
		return nil
	}
	request := uartWriteRequest{data: append([]byte(nil), data...)}
	select {
	case <-bridge.ctx.Done():
		return bridge.ctx.Err()
	case bridge.tx <- request:
		return nil
	}
}

func (bridge *uartBridge) QueueAndWait(data []byte) {
	if len(data) == 0 {
		return
	}
	request := uartWriteRequest{
		data: append([]byte(nil), data...),
		done: make(chan error, 1),
	}
	select {
	case <-bridge.ctx.Done():
		return
	case bridge.tx <- request:
	}

	timer := time.NewTimer(500 * time.Millisecond)
	defer timer.Stop()
	select {
	case <-bridge.ctx.Done():
	case <-timer.C:
	case <-request.done:
	}
}

func (bridge *uartBridge) sendLoop() {
	for {
		var request uartWriteRequest
		select {
		case <-bridge.ctx.Done():
			return
		case request = <-bridge.tx:
		}

		for {
			if bridge.connected.Load() {
				if err := bridge.client.SendUART(request.data); err == nil {
					if request.done != nil {
						request.done <- nil
					}
					break
				}
			}

			timer := time.NewTimer(25 * time.Millisecond)
			select {
			case <-bridge.ctx.Done():
				timer.Stop()
				if request.done != nil {
					request.done <- bridge.ctx.Err()
				}
				return
			case <-timer.C:
			}
		}
	}
}

func (bridge *uartBridge) Stop() {
	bridge.cancel()
	bridge.Detach()

	done := make(chan struct{})
	go func() {
		bridge.workers.Wait()
		close(done)
	}()
	select {
	case <-done:
	case <-time.After(time.Second):
	}
	_ = bridge.shadow.Close()
	select {
	case <-bridge.shadowDone:
	case <-time.After(time.Second):
	}
}

type uartSession struct {
	bridge       *uartBridge
	initialInput []byte
	clearScreen  bool

	stdin  io.Reader
	stdout io.Writer
	stderr io.Writer

	reason uartExitReason
}

func newUARTSession(
	bridge *uartBridge,
	initialInput []byte,
	clearScreen bool,
) *uartSession {
	return &uartSession{
		bridge:       bridge,
		initialInput: append([]byte(nil), initialInput...),
		clearScreen:  clearScreen,
		reason:       uartExitMenu,
	}
}

func (session *uartSession) SetStdin(reader io.Reader) {
	session.stdin = reader
}

func (session *uartSession) SetStdout(writer io.Writer) {
	session.stdout = writer
}

func (session *uartSession) SetStderr(writer io.Writer) {
	session.stderr = writer
}

func (session *uartSession) Run() error {
	if session.stdin == nil || session.stdout == nil {
		session.reason = uartExitError
		return errors.New("UART session has no terminal input or output")
	}

	fdOwner, ok := session.stdin.(interface{ Fd() uintptr })
	if !ok || !term.IsTerminal(int(fdOwner.Fd())) {
		session.reason = uartExitError
		return errors.New("UART session input is not a terminal")
	}

	previousState, err := term.MakeRaw(int(fdOwner.Fd()))
	if err != nil {
		session.reason = uartExitError
		return fmt.Errorf("enable raw terminal mode: %w", err)
	}
	defer term.Restore(int(fdOwner.Fd()), previousState)

	width, height, err := term.GetSize(int(fdOwner.Fd()))
	if err != nil {
		width, height = 80, 24
	}
	session.bridge.Attach(
		session.stdout,
		width,
		height,
		session.clearScreen,
	)
	defer session.bridge.Detach()

	if len(session.initialInput) > 0 {
		if err := session.bridge.Queue(session.initialInput); err != nil {
			session.reason = uartExitError
			return err
		}
	}
	return session.readInput()
}

func (session *uartSession) readInput() error {
	var decoder uv.EventDecoder
	buffer := make([]byte, 4096)
	for {
		count, err := session.stdin.Read(buffer)
		if count > 0 {
			data := buffer[:count]
			if index := uartMenuEventIndex(&decoder, data); index >= 0 {
				if index > 0 {
					session.bridge.QueueAndWait(data[:index])
				}
				session.reason = uartExitMenu
				return nil
			}

			if err := session.bridge.Queue(data); err != nil {
				session.reason = uartExitError
				return err
			}
		}
		if err != nil {
			if errors.Is(err, io.EOF) {
				session.reason = uartExitEOF
				return nil
			}
			session.reason = uartExitError
			return fmt.Errorf("read terminal input: %w", err)
		}
	}
}

func uartMenuEventIndex(decoder *uv.EventDecoder, data []byte) int {
	for offset := 0; offset < len(data); {
		count, event := decoder.Decode(data[offset:])
		if count <= 0 {
			return -1
		}
		if key, ok := event.(uv.KeyPressEvent); ok &&
			key.MatchString("ctrl+t") {
			return offset
		}
		offset += count
	}
	return -1
}

func saveUARTLog(log *uartLog) (string, error) {
	data := log.Snapshot()
	if len(data) == 0 {
		return "", errors.New("UART log is empty")
	}

	filename := fmt.Sprintf(
		"powermate_uart_%s.log",
		time.Now().Format("20060102_150405"),
	)
	file, err := os.OpenFile(
		filename,
		os.O_CREATE|os.O_EXCL|os.O_WRONLY,
		0o600,
	)
	if err != nil {
		return "", err
	}
	if _, err := file.Write(data); err != nil {
		file.Close()
		return "", err
	}
	if err := file.Close(); err != nil {
		return "", err
	}
	return filename, nil
}
