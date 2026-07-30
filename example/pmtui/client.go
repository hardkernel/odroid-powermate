package main

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

const maxHTTPResponseBytes = 256 * 1024

type client struct {
	baseURL    *url.URL
	httpClient *http.Client
	username   string
	password   string

	loginMu sync.Mutex
	tokenMu sync.RWMutex
	token   string

	uartMu      sync.RWMutex
	uartWriteMu sync.Mutex
	uartConn    *websocket.Conn
}

type loginResponse struct {
	Token string `json:"token"`
}

type controlStatus struct {
	Main bool `json:"load_12v_on"`
	USB  bool `json:"load_5v_on"`
}

type versionResponse struct {
	Version string `json:"version"`
}

type diagnostics struct {
	UptimeSeconds       uint64 `json:"uptime_seconds"`
	FreeHeapBytes       uint64 `json:"free_heap_bytes"`
	MinimumFreeHeap     uint64 `json:"minimum_free_heap_bytes"`
	LargestFreeBlock    uint64 `json:"largest_free_block_bytes"`
	HTTPClients         uint64 `json:"http_clients"`
	WebSocketClients    uint64 `json:"websocket_clients"`
	WebSocketQueueDepth uint64 `json:"websocket_queue_depth"`
	WebSocketQueueCap   uint64 `json:"websocket_queue_capacity"`
	WebSocketFailures   uint64 `json:"websocket_send_failures"`
	UARTBufferedBytes   uint64 `json:"uart_buffered_bytes"`
	UARTReceivedBytes   uint64 `json:"uart_received_bytes"`
	UARTFIFOOverflows   uint64 `json:"uart_fifo_overflows"`
	UARTBufferFull      uint64 `json:"uart_buffer_full_events"`
	UARTQueueDrops      uint64 `json:"uart_queue_drops"`
	StatusQueueDrops    uint64 `json:"status_queue_drops"`
	WiFiConnected       bool   `json:"wifi_connected"`
	WiFiRSSI            int32  `json:"wifi_rssi"`
	WiFiSTAState        string `json:"wifi_sta_state"`
	WiFiLastReason      string `json:"wifi_last_disconnect_reason"`
	WiFiLastReasonCode  int32  `json:"wifi_last_disconnect_reason_code"`
	WiFiReconnectMS     uint64 `json:"wifi_reconnect_backoff_ms"`
	WiFiHasConnected    bool   `json:"wifi_has_connected"`
	WiFiLastConnected   uint64 `json:"wifi_last_connected_uptime_seconds"`
	WiFiNetType         string `json:"wifi_net_type"`
	WiFiIPAddress       string `json:"wifi_ip_address"`
	WiFiGateway         string `json:"wifi_gateway"`
	WiFiNetmask         string `json:"wifi_netmask"`
}

type settingIPInfo struct {
	IP      string `json:"ip"`
	Gateway string `json:"gateway"`
	Subnet  string `json:"subnet"`
	DNS1    string `json:"dns1"`
	DNS2    string `json:"dns2"`
}

type deviceSettings struct {
	WiFiConnectionStatus string         `json:"wifi_connection_status"`
	WiFiFailureReason    string         `json:"wifi_failure_reason"`
	Mode                 string         `json:"mode"`
	NetworkType          string         `json:"net_type"`
	BaudRate             string         `json:"baudrate"`
	Period               string         `json:"period"`
	RestoreOutputState   bool           `json:"restore_output_state"`
	VINLimit             float64        `json:"vin_current_limit"`
	MAINLimit            float64        `json:"main_current_limit"`
	USBLimit             float64        `json:"usb_current_limit"`
	VINCriticalLimit     float64        `json:"vin_critical_current_limit"`
	MAINCriticalLimit    float64        `json:"main_critical_current_limit"`
	USBCriticalLimit     float64        `json:"usb_critical_current_limit"`
	Connected            bool           `json:"connected"`
	SSID                 string         `json:"ssid"`
	RSSI                 int32          `json:"rssi"`
	IP                   *settingIPInfo `json:"ip"`
}

type wifiAccessPoint struct {
	SSID     string `json:"ssid"`
	RSSI     int32  `json:"rssi"`
	AuthMode string `json:"authmode"`
}

type settingResponse struct {
	Status  string `json:"status"`
	Message string `json:"message"`
}

func newClient(host, username, password string) (*client, error) {
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

	return &client{
		baseURL: baseURL,
		httpClient: &http.Client{
			Timeout: 8 * time.Second,
		},
		username: username,
		password: password,
	}, nil
}

func (c *client) SetCredentials(username, password string) {
	c.loginMu.Lock()
	defer c.loginMu.Unlock()

	c.username = username
	c.password = password

	c.tokenMu.Lock()
	c.token = ""
	c.tokenMu.Unlock()
}

func (c *client) Username() string {
	c.loginMu.Lock()
	defer c.loginMu.Unlock()
	return c.username
}

func (c *client) Login(ctx context.Context) error {
	c.loginMu.Lock()
	defer c.loginMu.Unlock()

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

	var result loginResponse
	if err := decodeJSONResponse(response.Body, &result); err != nil {
		return fmt.Errorf("decode login response: %w", err)
	}
	if result.Token == "" {
		return errors.New("login response did not contain a token")
	}

	c.tokenMu.Lock()
	c.token = result.Token
	c.tokenMu.Unlock()
	return nil
}

func (c *client) GetVersion(ctx context.Context) (versionResponse, error) {
	var result versionResponse
	err := c.doJSON(ctx, http.MethodGet, "/api/version", nil, &result, true)
	return result, err
}

func (c *client) GetControl(ctx context.Context) (controlStatus, error) {
	var result controlStatus
	err := c.doJSON(ctx, http.MethodGet, "/api/control", nil, &result, true)
	return result, err
}

func (c *client) SetControl(ctx context.Context, payload map[string]bool) error {
	return c.doJSON(ctx, http.MethodPost, "/api/control", payload, nil, true)
}

func (c *client) GetDiagnostics(ctx context.Context) (diagnostics, error) {
	var result diagnostics
	err := c.doJSON(ctx, http.MethodGet, "/api/diagnostics", nil, &result, true)
	return result, err
}

func (c *client) SetSetting(ctx context.Context, payload map[string]any) error {
	var result settingResponse
	if err := c.doJSON(ctx, http.MethodPost, "/api/setting", payload, &result, true); err != nil {
		return err
	}
	if result.Status == "error" {
		if result.Message != "" {
			return errors.New(result.Message)
		}
		return errors.New("device rejected the setting")
	}
	return nil
}

func (c *client) GetSettings(ctx context.Context) (deviceSettings, error) {
	var result deviceSettings
	err := c.doJSON(ctx, http.MethodGet, "/api/setting", nil, &result, true)
	return result, err
}

func (c *client) ScanWiFi(ctx context.Context) ([]wifiAccessPoint, error) {
	var result []wifiAccessPoint
	err := c.doJSON(ctx, http.MethodGet, "/api/wifi/scan", nil, &result, true)
	return result, err
}

func (c *client) Reboot(ctx context.Context) error {
	return c.doJSON(ctx, http.MethodPost, "/api/reboot", nil, nil, true)
}

func (c *client) doJSON(
	ctx context.Context,
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
	if token := c.getToken(); token != "" {
		request.Header.Set("Authorization", "Bearer "+token)
	}

	response, err := c.httpClient.Do(request)
	if err != nil {
		return err
	}
	defer response.Body.Close()

	if response.StatusCode == http.StatusUnauthorized && retryAuth {
		if err := c.Login(ctx); err != nil {
			return err
		}
		return c.doJSON(ctx, method, path, requestBody, responseBody, false)
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
	if err := decoder.Decode(result); err != nil {
		return err
	}
	return nil
}

func (c *client) RunStatus(
	ctx context.Context,
	onMessage func(statusMessage),
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
			message, err := decodeStatusMessage(data)
			if err != nil {
				onState(true, fmt.Errorf("decode status message: %w", err))
				continue
			}
			onMessage(message)
		}
	}, onState)
}

func (c *client) RunUART(
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

func (c *client) SendUART(data []byte) error {
	c.uartMu.RLock()
	connection := c.uartConn
	c.uartMu.RUnlock()
	if connection == nil {
		return errors.New("UART WebSocket is not connected")
	}

	c.uartWriteMu.Lock()
	defer c.uartWriteMu.Unlock()
	return connection.WriteMessage(websocket.BinaryMessage, data)
}

func (c *client) runWebSocket(
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

		connection, response, err := c.dialWebSocket(ctx, path)
		if err == nil {
			backoff = time.Second
			onState(true, nil)
			sessionDone := make(chan struct{})
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
				if loginErr := c.Login(ctx); loginErr != nil {
					onState(false, loginErr)
				} else {
					continue
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

func (c *client) dialWebSocket(ctx context.Context, path string) (*websocket.Conn, *http.Response, error) {
	wsURL := *c.baseURL
	if wsURL.Scheme == "https" {
		wsURL.Scheme = "wss"
	} else {
		wsURL.Scheme = "ws"
	}
	wsURL.Path = strings.TrimRight(wsURL.Path, "/") + path
	query := wsURL.Query()
	query.Set("token", c.getToken())
	wsURL.RawQuery = query.Encode()

	return websocket.DefaultDialer.DialContext(ctx, wsURL.String(), nil)
}

func (c *client) endpoint(path string) string {
	endpoint := *c.baseURL
	endpoint.Path = strings.TrimRight(endpoint.Path, "/") + path
	return endpoint.String()
}

func (c *client) getToken() string {
	c.tokenMu.RLock()
	defer c.tokenMu.RUnlock()
	return c.token
}
