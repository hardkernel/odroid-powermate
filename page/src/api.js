/**
 * @file api.js
 * @description This module centralizes all API calls to the server's RESTful endpoints.
 * It abstracts the fetch logic, error handling, and JSON parsing for network and control operations.
 */

/**
 * Fetches the list of available Wi-Fi networks from the server.
 * @returns {Promise<Array<Object>>} A promise that resolves to an array of Wi-Fi access point objects.
 * @throws {Error} Throws an error if the network request fails.
 */
export async function fetchWifiScan() {
    const response = await fetch('/api/wifi/scan');
    if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
    return await response.json();
}

/**
 * Sends a request to connect to a specific Wi-Fi network.
 * @param {string} ssid The SSID of the network to connect to.
 * @param {string} password The password for the network.
 * @returns {Promise<Object>} A promise that resolves to the server's JSON response.
 * @throws {Error} Throws an error if the connection request fails.
 */
export async function postWifiConnect(ssid, password) {
    const response = await fetch('/api/setting', { // Updated URL
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ssid, password}),
    });
    if (!response.ok) {
        const errorText = await response.text();
        throw new Error(errorText || `Connection failed with status: ${response.status}`);
    }
    return await response.json();
}

/**
 * Posts updated network settings (e.g., static IP, DHCP, AP mode) to the server.
 * @param {Object} payload The settings object to be sent.
 * @returns {Promise<Response>} A promise that resolves to the raw fetch response.
 * @throws {Error} Throws an error if the request fails.
 */
export async function postNetworkSettings(payload) {
    const response = await fetch('/api/setting', { // Updated URL
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(payload),
    });
    if (!response.ok) {
        const errorText = await response.text();
        throw new Error(errorText || `Failed to apply settings with status: ${response.status}`);
    }
    return response;
}

/**
 * Posts the selected UART baud rate to the server.
 * @param {string} baudrate The selected baud rate.
 * @returns {Promise<Response>} A promise that resolves to the raw fetch response.
 * @throws {Error} Throws an error if the request fails.
 */
export async function postBaudRateSetting(baudrate) {
    const response = await fetch('/api/setting', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({baudrate}),
    });
    if (!response.ok) {
        const errorText = await response.text();
        throw new Error(errorText || `Failed to apply baudrate with status: ${response.status}`);
    }
    return response;
}

/**
 * Fetches the current network settings and Wi-Fi status from the server.
 * @returns {Promise<Object>} A promise that resolves to an object containing the current settings.
 * @throws {Error} Throws an error if the network request fails.
 */
export async function fetchSettings() {
    const response = await fetch('/api/setting'); // Updated URL
    if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
    return await response.json();
}

/**
 * Fetches the current status of the power control relays (12V and 5V).
 * @returns {Promise<Object>} A promise that resolves to an object with the power status.
 * @throws {Error} Throws an error if the network request fails.
 */
export async function fetchControlStatus() {
    const response = await fetch('/api/control');
    if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
    return await response.json();
}

/**
 * Sends a command to the server to control power functions (e.g., toggle relays, trigger reset).
 * @param {Object} command The control command object.
 * @returns {Promise<Response>} A promise that resolves to the raw fetch response.
 * @throws {Error} Throws an error if the request fails.
 */
export async function postControlCommand(command) {
    const response = await fetch('/api/control', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify(command)
    });
    if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
    return response;
}

/**
 * Fetches the firmware version from the server.
 * @returns {Promise<Object>} A promise that resolves to an object containing the version.
 * @throws {Error} Throws an error if the network request fails.
 */
export async function fetchVersion() {
    const response = await fetch('/api/version');
    if (!response.ok) throw new Error(`HTTP error! status: ${response.status}`);
    return await response.json();
}
