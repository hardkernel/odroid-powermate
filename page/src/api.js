/**
 * @file api.js
 * @description This module centralizes all API calls to the server's RESTful endpoints.
 * It abstracts the fetch logic, error handling, and JSON parsing for network and control operations.
 */

// Function to get authentication headers
export function getAuthHeaders() {
    const token = localStorage.getItem('authToken');
    if (token) {
        return { 'Authorization': `Bearer ${token}` };
    }
    return {};
}

// Global error handler for unauthorized responses
export async function handleResponse(response) {
    if (response.status === 401) {
        // Unauthorized, log out the user
        localStorage.removeItem('authToken');
        // Redirect to login or trigger a logout event
        // For now, we'll just reload the page, which will trigger the login screen
        window.location.reload();
        throw new Error('Unauthorized: Session expired or invalid token.');
    }
    if (!response.ok) {
        const errorText = await response.text();
        throw new Error(errorText || `HTTP error! status: ${response.status}`);
    }
    return response;
}

/**
 * Authenticates a user with the provided username and password.
 * @param {string} username The user's username.
 * @param {string} password The user's password.
 * @returns {Promise<Object>} A promise that resolves to the server's JSON response containing a token.
 * @throws {Error} Throws an error if the authentication fails.
 */
export async function login(username, password) {
    const response = await fetch('/login', {
        method: 'POST',
        headers: {'Content-Type': 'application/json'},
        body: JSON.stringify({ username, password }),
    });
    // Login function does not use handleResponse as it's for obtaining the token
    if (!response.ok) {
        const errorText = await response.text();
        throw new Error(errorText || `Login failed with status: ${response.status}`);
    }
    return await response.json();
}

/**
 * Fetches the list of available Wi-Fi networks from the server.
 * @returns {Promise<Array<Object>>} A promise that resolves to an array of Wi-Fi access point objects.
 * @throws {Error} Throws an error if the network request fails.
 */
export async function fetchWifiScan() {
    const response = await fetch('/api/wifi/scan', {
        headers: getAuthHeaders(),
    });
    return await handleResponse(response).then(res => res.json());
}

/**
 * Sends a request to connect to a specific Wi-Fi network.
 * @param {string} ssid The SSID of the network to connect to.
 * @param {string} password The password for the network.
 * @returns {Promise<Object>} A promise that resolves to the server's JSON response.
 * @throws {Error} Throws an error if the connection request fails.
 */
export async function postWifiConnect(ssid, password) {
    const response = await fetch('/api/setting', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            ...getAuthHeaders(),
        },
        body: JSON.stringify({ssid, password}),
    });
    return await handleResponse(response).then(res => res.json());
}

/**
 * Posts updated network settings (e.g., static IP, DHCP, AP mode) to the server.
 * @param {Object} payload The settings object to be sent.
 * @returns {Promise<Response>} A promise that resolves to the raw fetch response.
 * @throws {Error} Throws an error if the request fails.
 */
export async function postNetworkSettings(payload) {
    const response = await fetch('/api/setting', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            ...getAuthHeaders(),
        },
        body: JSON.stringify(payload),
    });
    return await handleResponse(response);
}

/**
 * Posts the selected UART baud rate to the server.
 * @param {string} baudrate The selected baud rate.
 * @returns {Promise<Response>} A promise that resolves to the raw fetch response.
 */
export async function postBaudRateSetting(baudrate) {
    const response = await fetch('/api/setting', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            ...getAuthHeaders(),
        },
        body: JSON.stringify({ baudrate }),
    });
    return await handleResponse(response);
}

/**
 * Posts the selected sensor period to the server.
 * @param {string} period The selected period in milliseconds.
 * @returns {Promise<Response>} A promise that resolves to the raw fetch response.
 */
export async function postPeriodSetting(period) {
    const response = await fetch('/api/setting', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            ...getAuthHeaders(),
        },
        body: JSON.stringify({ period }),
    });
    return await handleResponse(response);
}

/**
 * Fetches the current network settings and Wi-Fi status from the server.
 * @returns {Promise<Object>} A promise that resolves to an object containing the current settings.
 * @throws {Error} Throws an error if the network request fails.
 */
export async function fetchSettings() {
    const response = await fetch('/api/setting', {
        headers: getAuthHeaders(),
    });
    return await handleResponse(response).then(res => res.json());
}

/**
 * Fetches the current status of the power control relays (12V and 5V).
 * @returns {Promise<Object>} A promise that resolves to an object with the power status.
 * @throws {Error} Throws an error if the network request fails.
 */
export async function fetchControlStatus() {
    const response = await fetch('/api/control', {
        headers: getAuthHeaders(),
    });
    return await handleResponse(response).then(res => res.json());
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
        headers: {
            'Content-Type': 'application/json',
            ...getAuthHeaders(),
        },
        body: JSON.stringify(command)
    });
    return await handleResponse(response);
}

/**
 * Fetches the firmware version from the server.
 * @returns {Promise<Object>} A promise that resolves to an object containing the version.
 * @throws {Error} Throws an error if the network request fails.
 */
export async function fetchVersion() {
    const response = await fetch('/api/version', {
        headers: getAuthHeaders(),
    });
    return await handleResponse(response).then(res => res.json());
}

/**
 * Updates the user's username and password on the server.
 * @param {string} newUsername The new username.
 * @param {string} newPassword The new password.
 * @returns {Promise<Object>} A promise that resolves to the server's JSON response.
 * @throws {Error} Throws an error if the update fails.
 */
export async function updateUserSettings(newUsername, newPassword) {
    const response = await fetch('/api/setting', {
        method: 'POST',
        headers: {
            'Content-Type': 'application/json',
            ...getAuthHeaders(),
        },
        body: JSON.stringify({new_username: newUsername, new_password: newPassword}),
    });
    return await handleResponse(response).then(res => res.json());
}
