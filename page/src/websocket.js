/**
 * @file websocket.js
 * @description This module handles the WebSocket connection for real-time, two-way
 * communication with the server. It provides functions to initialize the connection
 * and send messages, including a heartbeat mechanism to detect disconnections.
 */

// The WebSocket instance, exported for potential direct access if needed.
export let websocket;

// The WebSocket server address, derived from the current page's host (hostname + port).
const baseGateway = `ws://${window.location.host}/ws`;

// Heartbeat related variables
let pingIntervalId = null;
let pongTimeoutId = null;
const HEARTBEAT_INTERVAL = 10000; // 10 seconds: How often to send a 'ping'
const HEARTBEAT_TIMEOUT = 5000; // 5 seconds: How long to wait for a 'pong' after sending a 'ping'

/**
 * Starts the heartbeat mechanism.
 * Sends a 'ping' message to the server at regular intervals and sets a timeout
 * to detect if a 'pong' response is not received.
 */
function startHeartbeat() {
    stopHeartbeat(); // Ensure any previous heartbeat is stopped before starting a new one

    pingIntervalId = setInterval(() => {
        if (websocket && websocket.readyState === WebSocket.OPEN) {
            websocket.send('ping');
            console.log('WebSocket: Ping sent.');

            // Set a timeout to check if a pong is received within HEARTBEAT_TIMEOUT
            pongTimeoutId = setTimeout(() => {
                console.warn('WebSocket: No pong received within timeout, closing connection.');
                // If no pong is received, close the connection. This will trigger the onClose handler.
                websocket.close();
            }, HEARTBEAT_TIMEOUT);
        }
    }, HEARTBEAT_INTERVAL);
}

/**
 * Stops the heartbeat mechanism by clearing the ping interval and pong timeout.
 */
function stopHeartbeat() {
    if (pingIntervalId) {
        clearInterval(pingIntervalId);
        pingIntervalId = null;
    }
    if (pongTimeoutId) {
        clearTimeout(pongTimeoutId);
        pongTimeoutId = null;
    }
}

/**
 * Initializes the WebSocket connection and sets up event handlers, including a heartbeat mechanism.
 * @param {Object} callbacks - An object containing callback functions for WebSocket events.
 * @param {function} [callbacks.onOpen] - Called when the connection is successfully opened.
 * @param {function} [callbacks.onClose] - Called when the connection is closed.
 * @param {function} [callbacks.onMessage] - Called when a message is received from the server (excluding 'pong' messages).
 * @param {function} [callbacks.onError] - Called when an error occurs with the WebSocket connection.
 */
export function initWebSocket({onOpen, onClose, onMessage, onError}) {
    const token = localStorage.getItem('authToken');
    let gateway = baseGateway;

    if (token) {
        gateway = `${baseGateway}?token=${token}`;
    }

    console.log(`Trying to open a WebSocket connection to ${gateway}...`);
    websocket = new WebSocket(gateway);
    // Set binary type to arraybuffer to handle raw binary data from the UART.
    websocket.binaryType = "arraybuffer";

    // Assign event handlers, wrapping user-provided callbacks to include heartbeat logic
    websocket.onopen = (event) => {
        console.log('WebSocket connection opened.');
        startHeartbeat(); // Start heartbeat on successful connection
        if (onOpen) {
            onOpen(event);
        }
    };

    websocket.onclose = (event) => {
        console.log('WebSocket connection closed:', event);
        stopHeartbeat(); // Stop heartbeat when connection closes
        if (onClose) {
            onClose(event);
        }
    };

    websocket.onmessage = (event) => {
        if (event.data === 'pong') {
            console.log('WebSocket: Pong received.');
            // Clear the timeout as pong was received, resetting for the next ping
            clearTimeout(pongTimeoutId);
            pongTimeoutId = null;
        } else {
            // If it's not a pong message, pass it to the user's onMessage callback
            if (onMessage) {
                onMessage(event);
            } else {
                console.log('WebSocket message received:', event.data);
            }
        }
    };

    websocket.onerror = (error) => {
        console.error('WebSocket error:', error);
        if (onError) {
            onError(error);
        }
    };
}

/**
 * Sends data over the WebSocket connection if it is open.
 * @param {string | ArrayBuffer} data - The data to send to the server.
 */
export function sendWebsocketMessage(data) {
    if (websocket && websocket.readyState === WebSocket.OPEN) {
        websocket.send(data);
    } else {
        console.warn('WebSocket is not open. Message not sent:', data);
    }
}