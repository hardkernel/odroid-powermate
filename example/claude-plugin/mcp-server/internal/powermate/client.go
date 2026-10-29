// Package powermate is a client for the ODROID PowerMate HTTP/WebSocket API.
//
// Derived from example/pmtui/client.go. Differences that matter for a long-running
// agent tool:
//   - the token can be persisted (TokenStore) so restarts do not consume one of the
//     firmware's four token slots;
//   - concurrent 401s refresh the token once instead of once per caller;
//   - UART writes are split below the firmware's 2048-byte WebSocket frame limit.
package powermate

import (
	"bytes"
	"context"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"net/http"
	"net/url"
	"strings"
	"sync"
	"time"

	"github.com/gorilla/websocket"
)

const (
	maxHTTPResponseBytes = 256 * 1024
	webSocketPongWait    = 15 * time.Second
	webSocketPingPeriod  = 5 * time.Second
	webSocketWriteWait   = 2 * time.Second

	// The firmware receives WebSocket frames into a 2048-byte buffer and closes the
	// session on anything larger (main/service/ws.c BUF_SIZE).
	UARTMaxChunk = 1024
)

// TokenStore persists the session token between process restarts.
type TokenStore interface {
	Load() string
	Save(token string)
}

type Client struct {
	baseURL    *url.URL
	httpClient *http.Client
	username   string
	password   string
	tokens     TokenStore

	loginMu sync.Mutex
	tokenMu sync.RWMutex
	token   string

	uartMu      sync.RWMutex
	uartWriteMu sync.Mutex
	uartConn    *websocket.Conn
}

type ControlStatus struct {
	Main bool `json:"load_12v_on"`
	USB  bool `json:"load_5v_on"`
}

type VersionResponse struct {
	Version string `json:"version"`
}

type Settings struct {
	BaudRate           string `json:"baudrate"`
	Period             string `json:"period"`
	RestoreOutputState bool   `json:"restore_output_state"`
}

func NewClient(host, username, password string, tokens TokenStore) (*Client, error) {
	host = strings.TrimSpace(host)
	if host == "" {
		return nil, errors.New("host is required")
	}
	if !strings.Contains(host, "://") {
		host = "http://" + host
	}

	baseURL, err := url.Parse(host)
	if err != nil {
		return nil, fmt.Errorf("parse host: %w", err)
	}
	if baseURL.Scheme != "http" && baseURL.Scheme != "https" {
		return nil, fmt.Errorf("unsupported URL scheme %q", baseURL.Scheme)
	}
	if baseURL.Host == "" {
		return nil, errors.New("host does not contain an address")
	}
	baseURL.Path = strings.TrimRight(baseURL.Path, "/")

	c := &Client{
		baseURL:    baseURL,
		httpClient: &http.Client{Timeout: 8 * time.Second},
		username:   username,
		password:   password,
		tokens:     tokens,
	}
	if tokens != nil {
		c.token = tokens.Load()
	}
	return c, nil
}

// Host returns the device address as configured.
func (c *Client) Host() string {
	return c.baseURL.Host
}

// refreshToken logs in unless another caller already replaced the stale token.
func (c *Client) refreshToken(ctx context.Context, stale string) error {
	c.loginMu.Lock()
	defer c.loginMu.Unlock()

	if current := c.getToken(); current != "" && current != stale {
		return nil
	}
	// The firmware closes the socket after every error reply (including the 401 that
	// brought us here), but the transport may still hand that dead keep-alive
	// connection to the login POST, which is not retried. Start from fresh connections.
	c.httpClient.CloseIdleConnections()

	payload, err := json.Marshal(map[string]string{
		"username": c.username,
		"password": c.password,
	})
	if err != nil {
		return err
	}

	request, err := http.NewRequestWithContext(ctx, http.MethodPost, c.endpoint("/login"), bytes.NewReader(payload))
	if err != nil {
		return err
	}
	request.Header.Set("Content-Type", "application/json")

	response, err := c.httpClient.Do(request)
	if err != nil {
		return fmt.Errorf("login request: %w", err)
	}
	defer response.Body.Close()

	if response.StatusCode != http.StatusOK {
		body, _ := io.ReadAll(io.LimitReader(response.Body, 4096))
		return fmt.Errorf("login failed: HTTP %d: %s", response.StatusCode, strings.TrimSpace(string(body)))
	}

	var result struct {
		Token string `json:"token"`
	}
	if err := decodeJSONResponse(response.Body, &result); err != nil {
		return fmt.Errorf("decode login response: %w", err)
	}
	if result.Token == "" {
		return errors.New("login response did not contain a token")
	}

	c.tokenMu.Lock()
	c.token = result.Token
	c.tokenMu.Unlock()
	if c.tokens != nil {
		c.tokens.Save(result.Token)
	}
	return nil
}

func (c *Client) GetVersion(ctx context.Context) (VersionResponse, error) {
	var result VersionResponse
	err := c.doJSON(ctx, http.MethodGet, "/api/version", nil, &result, true)
	return result, err
}

func (c *Client) GetControl(ctx context.Context) (ControlStatus, error) {
	var result ControlStatus
	err := c.doJSON(ctx, http.MethodGet, "/api/control", nil, &result, true)
	return result, err
}

// SetControl accepts load_12v_on, load_5v_on, power_trigger and reset_trigger.
func (c *Client) SetControl(ctx context.Context, payload map[string]bool) error {
	return c.doJSON(ctx, http.MethodPost, "/api/control", payload, nil, true)
}

func (c *Client) GetSettings(ctx context.Context) (Settings, error) {
	var result Settings
	err := c.doJSON(ctx, http.MethodGet, "/api/setting", nil, &result, true)
	return result, err
}

// RawSettings returns GET /api/setting as decoded JSON, every field included.
func (c *Client) RawSettings(ctx context.Context) (map[string]any, error) {
	var result map[string]any
	err := c.doJSON(ctx, http.MethodGet, "/api/setting", nil, &result, true)
	return result, err
}

// PostSettings sends a partial POST /api/setting. The firmware answers HTTP 200 with
// "status":"error" when it rejects part of the request; that is returned as an
// error together with the full response.
func (c *Client) PostSettings(ctx context.Context, payload map[string]any) (map[string]any, error) {
	var result map[string]any
	if err := c.doJSON(ctx, http.MethodPost, "/api/setting", payload, &result, true); err != nil {
		return result, err
	}
	if result["status"] == "error" {
		msg, _ := result["message"].(string)
		if msg == "" {
			msg = "device rejected part of the request"
		}
		return result, errors.New(msg)
	}
	return result, nil
}

type AccessPoint struct {
	SSID     string `json:"ssid"`
	RSSI     int32  `json:"rssi"`
	AuthMode string `json:"authmode"`
}

func (c *Client) ScanWiFi(ctx context.Context) ([]AccessPoint, error) {
	var result []AccessPoint
	// A scan blocks the firmware for several seconds.
	ctx, cancel := context.WithTimeout(ctx, 20*time.Second)
	defer cancel()
	err := c.doJSONClient(ctx, &http.Client{Timeout: 20 * time.Second}, http.MethodGet, "/api/wifi/scan", nil, &result, true)
	return result, err
}

func (c *Client) Diagnostics(ctx context.Context) (map[string]any, error) {
	var result map[string]any
	err := c.doJSON(ctx, http.MethodGet, "/api/diagnostics", nil, &result, true)
	return result, err
}

func (c *Client) Reboot(ctx context.Context) error {
	return c.doJSON(ctx, http.MethodPost, "/api/reboot", nil, nil, true)
}

// SetCredentials replaces the login used for future token refreshes. The current
// token stays valid: the firmware does not revoke tokens on a password change.
func (c *Client) SetCredentials(username, password string) {
	c.loginMu.Lock()
	defer c.loginMu.Unlock()
	c.username = username
	c.password = password
}

func (c *Client) doJSON(
	ctx context.Context,
	method string,
	path string,
	requestBody any,
	responseBody any,
	retryAuth bool,
) error {
	return c.doJSONClient(ctx, c.httpClient, method, path, requestBody, responseBody, retryAuth)
}

func (c *Client) doJSONClient(
	ctx context.Context,
	httpClient *http.Client,
	method string,
	path string,
	requestBody any,
	responseBody any,
	retryAuth bool,
) error {
	var encoded []byte
	var err error
	if requestBody != nil {
		encoded, err = json.Marshal(requestBody)
		if err != nil {
			return err
		}
	}

	request, err := http.NewRequestWithContext(ctx, method, c.endpoint(path), bytes.NewReader(encoded))
	if err != nil {
		return err
	}
	if requestBody != nil {
		request.Header.Set("Content-Type", "application/json")
	}
	token := c.getToken()
	if token != "" {
		request.Header.Set("Authorization", "Bearer "+token)
	}

	response, err := httpClient.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()

	if response.StatusCode == http.StatusUnauthorized && retryAuth {
		io.Copy(io.Discard, io.LimitReader(response.Body, maxHTTPResponseBytes))
		if err := c.refreshToken(ctx, token); err != nil {
			return err
		}
		return c.doJSONClient(ctx, httpClient, method, path, requestBody, responseBody, false)
	}
	if response.StatusCode < 200 || response.StatusCode >= 300 {
		body, _ := io.ReadAll(io.LimitReader(response.Body, 4096))
		return fmt.Errorf("HTTP %d: %s", response.StatusCode, strings.TrimSpace(string(body)))
	}
	if responseBody == nil {
		io.Copy(io.Discard, io.LimitReader(response.Body, maxHTTPResponseBytes))
		return nil
	}
	return decodeJSONResponse(response.Body, responseBody)
}

func decodeJSONResponse(reader io.Reader, result any) error {
	decoder := json.NewDecoder(io.LimitReader(reader, maxHTTPResponseBytes))
	return decoder.Decode(result)
}

// RunStatus keeps the /ws status stream connected until ctx is done.
func (c *Client) RunStatus(
	ctx context.Context,
	onMessage func(StatusMessage),
	onState func(bool, error),
) {
	c.runWebSocket(ctx, "/ws", func(connection *websocket.Conn) error {
		for {
			messageType, data, err := connection.ReadMessage()
			if err != nil {
				return err
			}
			if messageType != websocket.BinaryMessage {
				continue
			}
			message, err := DecodeStatusMessage(data)
			if err != nil {
				onState(true, fmt.Errorf("decode status message: %w", err))
				continue
			}
			onMessage(message)
		}
	}, onState)
}

// RunUART keeps the raw /uart stream connected until ctx is done.
func (c *Client) RunUART(
	ctx context.Context,
	onData func([]byte),
	onState func(bool, error),
) {
	c.runWebSocket(ctx, "/uart", func(connection *websocket.Conn) error {
		c.uartMu.Lock()
		c.uartConn = connection
		c.uartMu.Unlock()
		defer func() {
			c.uartMu.Lock()
			if c.uartConn == connection {
				c.uartConn = nil
			}
			c.uartMu.Unlock()
		}()

		for {
			messageType, data, err := connection.ReadMessage()
			if err != nil {
				return err
			}
			if messageType == websocket.BinaryMessage || messageType == websocket.TextMessage {
				onData(data)
			}
		}
	}, onState)
}

// SendUART writes data to the target's UART in frames the firmware accepts.
func (c *Client) SendUART(data []byte) error {
	c.uartMu.RLock()
	connection := c.uartConn
	c.uartMu.RUnlock()
	if connection == nil {
		return errors.New("UART WebSocket is not connected")
	}

	c.uartWriteMu.Lock()
	defer c.uartWriteMu.Unlock()
	for len(data) > 0 {
		n := min(len(data), UARTMaxChunk)
		_ = connection.SetWriteDeadline(time.Now().Add(webSocketWriteWait))
		if err := connection.WriteMessage(websocket.BinaryMessage, data[:n]); err != nil {
			return err
		}
		data = data[n:]
	}
	return nil
}

func (c *Client) runWebSocket(
	ctx context.Context,
	path string,
	session func(*websocket.Conn) error,
	onState func(bool, error),
) {
	backoff := time.Second
	for {
		if ctx.Err() != nil {
			return
		}

		token := c.getToken()
		connection, response, err := c.dialWebSocket(ctx, path, token)
		if err == nil {
			backoff = time.Second
			onState(true, nil)
			sessionDone := make(chan struct{})
			c.startWebSocketHeartbeat(ctx, connection, sessionDone)
			go func() {
				select {
				case <-ctx.Done():
					connection.Close()
				case <-sessionDone:
				}
			}()
			err = session(connection)
			close(sessionDone)
			connection.Close()
			if ctx.Err() != nil {
				return
			}
			onState(false, err)
		} else {
			if response != nil && response.Body != nil {
				response.Body.Close()
			}
			if response != nil && response.StatusCode == http.StatusUnauthorized {
				// Redial right away only when this call actually obtained a new token;
				// otherwise fall through to the backoff so a persistent 401 cannot spin.
				if loginErr := c.refreshToken(ctx, token); loginErr != nil {
					onState(false, loginErr)
				} else if c.getToken() != token {
					continue
				} else {
					onState(false, err)
				}
			} else {
				onState(false, err)
			}
		}

		timer := time.NewTimer(backoff)
		select {
		case <-ctx.Done():
			timer.Stop()
			return
		case <-timer.C:
		}
		if backoff < 16*time.Second {
			backoff *= 2
		}
	}
}

func (c *Client) startWebSocketHeartbeat(
	ctx context.Context,
	connection *websocket.Conn,
	done <-chan struct{},
) {
	_ = connection.SetReadDeadline(time.Now().Add(webSocketPongWait))
	connection.SetPongHandler(func(string) error {
		return connection.SetReadDeadline(time.Now().Add(webSocketPongWait))
	})

	go func() {
		ticker := time.NewTicker(webSocketPingPeriod)
		defer ticker.Stop()

		for {
			select {
			case <-ctx.Done():
				return
			case <-done:
				return
			case <-ticker.C:
				deadline := time.Now().Add(webSocketWriteWait)
				if err := connection.WriteControl(websocket.PingMessage, nil, deadline); err != nil {
					_ = connection.Close()
					return
				}
			}
		}
	}()
}

func (c *Client) dialWebSocket(ctx context.Context, path, token string) (*websocket.Conn, *http.Response, error) {
	wsURL := *c.baseURL
	if wsURL.Scheme == "https" {
		wsURL.Scheme = "wss"
	} else {
		wsURL.Scheme = "ws"
	}
	wsURL.Path = strings.TrimRight(wsURL.Path, "/") + path
	query := wsURL.Query()
	query.Set("token", token)
	wsURL.RawQuery = query.Encode()

	return websocket.DefaultDialer.DialContext(ctx, wsURL.String(), nil)
}

func (c *Client) endpoint(path string) string {
	endpoint := *c.baseURL
	endpoint.Path = strings.TrimRight(endpoint.Path, "/") + path
	return endpoint.String()
}

func (c *Client) getToken() string {
	c.tokenMu.RLock()
	defer c.tokenMu.RUnlock()
	return c.token
}
