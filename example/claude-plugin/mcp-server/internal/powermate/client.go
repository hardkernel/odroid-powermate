// SPDX-FileCopyrightText: 2026 Hardkernel Co., Ltd.
// SPDX-License-Identifier: Apache-2.0

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
	stableSession        = 10 * time.Second

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
	urlMu     sync.RWMutex
	baseURL   *url.URL
	resolveMu sync.Mutex
	resolved  bool // the scheme (http or https) is known

	httpClient *http.Client
	transport  *http.Transport
	wsDialer   *websocket.Dialer
	pin        string
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

// NewClient creates a client for the PowerMate at host ("192.168.4.1", "http://...",
// "https://..."). pin is the SHA-256 fingerprint of its certificate when it serves a
// self-signed one over HTTPS; empty trusts what this computer trusts.
//
// Without https://, the first request finds out whether the device runs HTTP or HTTPS
// by asking without credentials; nothing secret is sent before the scheme is known.
func NewClient(host, username, password string, tokens TokenStore, pin string) (*Client, error) {
	host = strings.TrimSpace(host)
	if host == "" {
		return nil, errors.New("host is required")
	}
	pin, err := NormalizeFingerprint(pin)
	if err != nil {
		return nil, err
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

	tlsConfig := newTLSConfig(baseURL.Host, pin)
	transport := http.DefaultTransport.(*http.Transport).Clone()
	transport.TLSClientConfig = tlsConfig
	c := &Client{
		baseURL:   baseURL,
		resolved:  baseURL.Scheme == "https",
		transport: transport,
		pin:       pin,
		httpClient: &http.Client{
			Timeout:   8 * time.Second,
			Transport: transport,
			// Never follow a redirect: it would resend the token, possibly in clear.
			CheckRedirect: func(*http.Request, []*http.Request) error { return http.ErrUseLastResponse },
		},
		wsDialer: &websocket.Dialer{
			Proxy:            http.ProxyFromEnvironment,
			HandshakeTimeout: 45 * time.Second,
			TLSClientConfig:  tlsConfig,
		},
		username: username,
		password: password,
		tokens:   tokens,
	}
	if tokens != nil {
		c.token = tokens.Load()
	}
	return c, nil
}

// Host returns the device address (host[:port]).
func (c *Client) Host() string {
	return c.base().Host
}

// URL returns the device's base URL, https:// once the device was found to serve HTTPS.
func (c *Client) URL() string {
	base := c.base()
	return base.String()
}

// Pin returns the pinned certificate fingerprint, if any.
func (c *Client) Pin() string {
	return c.pin
}

func (c *Client) base() url.URL {
	c.urlMu.RLock()
	defer c.urlMu.RUnlock()
	return *c.baseURL
}

func (c *Client) isResolved() bool {
	c.urlMu.RLock()
	defer c.urlMu.RUnlock()
	return c.resolved
}

func (c *Client) unresolve() {
	c.urlMu.Lock()
	defer c.urlMu.Unlock()
	if c.baseURL.Scheme == "http" {
		c.resolved = false
	}
}

// resolve finds out once whether the device serves HTTP or HTTPS: a GET without
// credentials that is not followed. With HTTPS on, the device's port 80 answers every
// GET with a redirect to https://; only one to the same host is accepted.
func (c *Client) resolve(ctx context.Context) error {
	if c.isResolved() {
		return nil
	}
	c.resolveMu.Lock()
	defer c.resolveMu.Unlock()
	if c.isResolved() {
		return nil
	}
	base := c.base()
	probe := base
	probe.Path = strings.TrimRight(probe.Path, "/") + "/api/version"
	request, err := http.NewRequestWithContext(ctx, http.MethodGet, probe.String(), nil)
	if err != nil {
		return err
	}
	response, err := c.httpClient.Do(request)
	if err != nil {
		return err
	}
	io.Copy(io.Discard, io.LimitReader(response.Body, maxHTTPResponseBytes))
	response.Body.Close()

	if response.StatusCode >= 300 && response.StatusCode < 400 {
		location, err := response.Location()
		if err != nil {
			return fmt.Errorf("redirect without a location: %w", err)
		}
		if location.Scheme != "https" || location.Hostname() != base.Hostname() {
			return fmt.Errorf("the device redirects to %s; only https:// on the same host is followed", location)
		}
		base.Scheme = "https"
		base.Host = location.Host
	}
	c.urlMu.Lock()
	c.baseURL = &base
	c.resolved = true
	c.urlMu.Unlock()
	return nil
}

// explain adds what to do to errors that have a likely cause on the PowerMate side.
func (c *Client) explain(err error) error {
	var certErr *CertError
	if err == nil || errors.As(err, &certErr) {
		return err
	}
	if base := c.base(); base.Scheme == "https" && strings.Contains(err.Error(), "connection refused") {
		return fmt.Errorf("%w (nothing answers HTTPS at %s: if HTTPS was turned off on the PowerMate, use http://%s)",
			err, base.Host, base.Hostname())
	}
	return err
}

// CloseIdleConnections drops idle keep-alive connections, which still hold one of
// the firmware's few sockets.
func (c *Client) CloseIdleConnections() {
	c.httpClient.CloseIdleConnections()
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
	if err := c.resolve(ctx); err != nil {
		return c.explain(err)
	}

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
		return fmt.Errorf("login request: %w", c.explain(err))
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
	scanClient := &http.Client{Timeout: 20 * time.Second, Transport: c.transport, CheckRedirect: c.httpClient.CheckRedirect}
	err := c.doJSONClient(ctx, scanClient, http.MethodGet, "/api/wifi/scan", nil, &result, true)
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
	if err := c.resolve(ctx); err != nil {
		return c.explain(err)
	}
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
		return c.explain(err)
	}
	defer response.Body.Close()

	if response.StatusCode >= 300 && response.StatusCode < 400 && retryAuth {
		// HTTPS was turned on since the scheme was resolved: find it again.
		io.Copy(io.Discard, io.LimitReader(response.Body, maxHTTPResponseBytes))
		c.unresolve()
		return c.doJSONClient(ctx, httpClient, method, path, requestBody, responseBody, false)
	}
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
			onState(true, nil)
			connectedAt := time.Now()
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
			// When the firmware runs out of sockets it closes the least recently used
			// one, often a WebSocket. Redialing at once would just push out another
			// client, which redials too, so keep backing off unless the session lasted.
			if time.Since(connectedAt) > stableSession {
				backoff = time.Second
			}
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
	if err := c.resolve(ctx); err != nil {
		return nil, nil, c.explain(err)
	}
	wsURL := c.base()
	if wsURL.Scheme == "https" {
		wsURL.Scheme = "wss"
	} else {
		wsURL.Scheme = "ws"
	}
	wsURL.Path = strings.TrimRight(wsURL.Path, "/") + path
	query := wsURL.Query()
	query.Set("token", token)
	wsURL.RawQuery = query.Encode()

	conn, response, err := c.wsDialer.DialContext(ctx, wsURL.String(), nil)
	if response != nil && response.StatusCode >= 300 && response.StatusCode < 400 {
		c.unresolve() // HTTPS was turned on: the next attempt finds it
	}
	return conn, response, c.explain(err)
}

func (c *Client) endpoint(path string) string {
	endpoint := c.base()
	endpoint.Path = strings.TrimRight(endpoint.Path, "/") + path
	return endpoint.String()
}

func (c *Client) getToken() string {
	c.tokenMu.RLock()
	defer c.tokenMu.RUnlock()
	return c.token
}
