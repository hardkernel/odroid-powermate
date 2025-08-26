# ODROID Remote API Documentation

This document outlines the HTTP REST and WebSocket APIs for communication between the web interface and the ESP32 device.

---

## WebSocket API

The WebSocket API provides a full-duplex communication channel for real-time data, such as sensor metrics and the interactive serial console.

**Endpoint**: `/ws`

### Server-to-Client Messages

The server pushes messages to the client, which can be either JSON objects or raw binary data. JSON messages always contain a `type` field to identify the payload.

#### JSON Messages

| Type          | Description                                                                     | Payload Example                                                                                                      |
|---------------|---------------------------------------------------------------------------------|----------------------------------------------------------------------------------------------------------------------|
| `sensor_data` | Pushed periodically (e.g., every second) with the latest power metrics.         | `{"type":"sensor_data", "voltage":12.01, "current":1.52, "power":18.25, "uptime_sec":3600, "timestamp": 1672531200}` |
| `wifi_status` | Pushed periodically or on change to update the current Wi-Fi connection status. | `{"type":"wifi_status", "connected":true, "ssid":"MyHome_WiFi", "rssi":-65}`                                         |

**Field Descriptions:**
- `sensor_data`:
  - `voltage` (float): The measured voltage in Volts (V).
  - `current` (float): The measured current in Amperes (A).
  - `power` (float): The calculated power in Watts (W).
  - `uptime_sec` (integer): The system uptime in seconds.
  - `timestamp` (integer): A Unix timestamp (seconds) of when the measurement was taken.
- `wifi_status`:
  - `connected` (boolean): `true` if connected to a Wi-Fi network, `false` otherwise.
  - `ssid` (string): The SSID of the connected network. Null if not connected.
  - `rssi` (integer): The Received Signal Strength Indicator in dBm. Null if not connected.

#### Raw Binary Data

- **Description**: Raw binary data from the ODROID's serial (UART) port is forwarded directly to the client. This is used to display the terminal output.
- **Payload**: `(binary data)`

### Client-to-Server Messages

The client primarily sends raw binary data, which is interpreted as terminal input.

- **Description**: Raw binary data representing user input from the web terminal. This data is forwarded directly to the ODROID's serial (UART) port.
- **Payload**: `(binary data)`

---

## HTTP REST API

The REST API is used for configuration and to trigger specific actions. All request and response bodies are in `application/json` format.

### Endpoint: `/api/control`

Manages power relays and system actions.

#### `GET /api/control`

Retrieves the current status of the power relays.

- **Success Response (200 OK)**
  ```json
  {
    "load_12v_on": true,
    "load_5v_on": false
  }
  ```
  - `load_12v_on` (boolean): The state of the main 12V power relay.
  - `load_5v_on` (boolean): The state of the 5V USB power relay.

#### `POST /api/control`

Sets the state of power relays or triggers a power action. You can send one or more commands in a single request.

- **Request Body Examples**:
  - To turn the main power on:
    ```json
    { "load_12v_on": true }
    ```
  - To trigger a system reset:
    ```json
    { "reset_trigger": true }
    ```
  - To toggle the power button:
    ```json
    { "power_trigger": true }
    ```

- **Request Fields**:
  - `load_12v_on` (boolean, optional): Sets the state of the 12V relay.
  - `load_5v_on` (boolean, optional): Sets the state of the 5V relay.
  - `reset_trigger` (boolean, optional): If `true`, momentarily triggers the reset button. The action is triggered only on a `true` value.
  - `power_trigger` (boolean, optional): If `true`, momentarily triggers the power button. The action is triggered only on a `true` value.

- **Success Response**: `204 No Content`

---

### Endpoint: `/api/wifi`

Manages all Wi-Fi and network-related configurations.

#### `GET /api/wifi`

Retrieves the complete current network configuration.

- **Success Response (200 OK)**
  ```json
  {
    "connected": true,
    "ssid": "MyHome_WiFi",
    "mode": "apsta",
    "net_type": "static",
    "ip": {
        "ip": "192.168.1.100",
        "gateway": "192.168.1.1",
        "subnet": "255.255.255.0",
        "dns1": "8.8.8.8",
        "dns2": "8.8.4.4"
    }
  }
  ```
- **Response Fields**:
  - `connected` (boolean): Current Wi-Fi connection state.
  - `ssid` (string): The SSID of the connected network.
  - `mode` (string): The current Wi-Fi mode (`"sta"` or `"apsta"`).
  - `net_type` (string): The network type (`"dhcp"` or `"static"`).
  - `ip` (object): Contains IP configuration details. Present even if using DHCP (may show last-leased IP).
    - `ip` (string): The device's IP address.
    - `gateway` (string): The network gateway address.
    - `subnet` (string): The network subnet mask.
    - `dns1` (string): The primary DNS server address.
    - `dns2` (string): The secondary DNS server address.

#### `POST /api/wifi`

This is a multi-purpose endpoint. The server determines the action based on the fields provided in the request body.

- **Action: Connect to a Wi-Fi Network**
  - **Request Body**:
    ```json
    {
      "ssid": "MyHome_WiFi",
      "password": "my_secret_password"
    }
    ```
  - **Success Response (200 OK)**:
    ```json
    { "status": "connection_initiated" }
    ```

- **Action: Configure Network Type (DHCP/Static)**
  - **Request Body (for DHCP)**:
    ```json
    { "net_type": "dhcp" }
    ```
  - **Request Body (for Static IP)**:
    *Note: The `ip` object structure is consistent with the `GET /api/wifi` response.*
    ```json
    {
      "net_type": "static",
      "ip": {
        "ip": "192.168.1.100",
        "gateway": "192.168.1.1",
        "subnet": "255.255.255.0",
        "dns1": "8.8.8.8",
        "dns2": "8.8.4.4"
      }
    }
    ```
  - **Success Response**: `204 No Content`

- **Action: Configure Wi-Fi Mode (STA/APSTA)**
  - **Request Body (for STA mode)**:
    ```json
    { "mode": "sta" }
    ```
  - **Request Body (for AP+STA mode)**:
    ```json
    {
        "mode": "apsta",
        "ap_ssid": "ODROID-Remote-AP",
        "ap_password": "hardkernel"
    }
    ```
    *Note: `ap_password` is optional. If omitted, the AP will be open.*
  - **Success Response**: `204 No Content`

---

### Endpoint: `/api/wifi/scan`

Scans for available Wi-Fi networks.

#### `GET /api/wifi/scan`

- **Success Response (200 OK)**: Returns a JSON array of found networks.
  ```json
  [
    {
      "ssid": "MyHome_WiFi",
      "rssi": -55,
      "authmode": "WPA2_PSK"
    },
    {
      "ssid": "GuestNetwork",
      "rssi": -78,
      "authmode": "OPEN"
    }
  ]
  ```
- **Response Fields**:
  - `ssid` (string): The network's Service Set Identifier.
  - `rssi` (integer): Signal strength in dBm.
  - `authmode` (string): The authentication mode (e.g., `"OPEN"`, `"WPA_PSK"`, `"WPA2_PSK"`).

---

### General Error Responses

In case of an error, the server will respond with an appropriate HTTP status code.

- **`400 Bad Request`**: The request is malformed, contains invalid parameters, or is otherwise incorrect. The response body may contain a JSON object with more details.
  ```json
  {
    "error": "Invalid request body"
  }
  ```
- **`404 Not Found`**: The requested endpoint does not exist.
- **`500 Internal Server Error`**: The server encountered an unexpected condition that prevented it from fulfilling the request.
