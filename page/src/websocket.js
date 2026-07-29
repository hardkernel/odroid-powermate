/**
 * @file websocket.js
 * @description This module handles the WebSocket connection for real-time, two-way
 * communication with the server. It provides functions to initialize the connection
 * and send messages, including a heartbeat mechanism to detect disconnections.
 */

// The WebSocket instance, exported for potential direct access if needed.
export let websocket;

// The WebSocket server address, derived from the current page's host (hostname + port).
const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
const baseGateway = `${protocol}//${window.location.host}/ws`;

// Heartbeat related variables
let heartbeatSocket;
let pingIntervalId = null;
let pongTimeoutId = null;
let connectingSocket;
let connectionTimeoutId = null;
const CONNECTION_TIMEOUT = 10000; // 10 seconds: How long to wait for the WebSocket handshake
const HEARTBEAT_INTERVAL = 10000; // 10 seconds: How often to send a 'ping'
const HEARTBEAT_TIMEOUT = 5000; // 5 seconds: How long to wait for a 'pong' after sending a 'ping'

function startConnectionTimeout(socket, onTimeout) {
    stopConnectionTimeout();
    connectingSocket = socket;
    connectionTimeoutId = setTimeout(() => {
        if (connectingSocket !== socket) return;
        console.warn('WebSocket: Connection attempt timed out.');
        onTimeout();
    }, CONNECTION_TIMEOUT);
}

function stopConnectionTimeout(socket) {
    if (socket && connectingSocket !== socket) return;

    if (connectionTimeoutId) {
        clearTimeout(connectionTimeoutId);
        connectionTimeoutId = null;
    }
    connectingSocket = undefined;
}

/**
 * Starts the heartbeat mechanism.
 * Sends a 'ping' message to the server at regular intervals and sets a timeout
 * to detect if a 'pong' response is not received.
 */
function startHeartbeat(socket, onTimeout) {
    stopHeartbeat(); // Ensure any previous heartbeat is stopped before starting a new one
    heartbeatSocket = socket;

    pingIntervalId = setInterval(() => {
        if (heartbeatSocket !== socket || socket.readyState !== WebSocket.OPEN) return;

        try {
            socket.send('ping');
        } catch (error) {
            console.warn('WebSocket: Failed to send heartbeat ping.', error);
            onTimeout();
            return;
        }

        // Set a timeout to check if a pong is received within HEARTBEAT_TIMEOUT
        pongTimeoutId = setTimeout(() => {
            if (heartbeatSocket !== socket) return;
            console.warn('WebSocket: No pong received within timeout.');
            onTimeout();
        }, HEARTBEAT_TIMEOUT);
    }, HEARTBEAT_INTERVAL);
}

/**
 * Stops the heartbeat mechanism by clearing the ping interval and pong timeout.
 */
function stopHeartbeat(socket) {
    if (socket && heartbeatSocket !== socket) return;

    if (pingIntervalId) {
        clearInterval(pingIntervalId);
        pingIntervalId = null;
    }
    if (pongTimeoutId) {
        clearTimeout(pongTimeoutId);
        pongTimeoutId = null;
    }
    heartbeatSocket = undefined;
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
    if (websocket &&
        (websocket.readyState === WebSocket.OPEN || websocket.readyState === WebSocket.CONNECTING)) {
        return;
    }

    const token = localStorage.getItem('authToken');
    let gateway = baseGateway;

    if (token) {
        gateway = `${baseGateway}?token=${token}`;
    }

    console.log(`Trying to open a WebSocket connection to ${gateway}...`);
    const socket = new WebSocket(gateway);
    let closeNotified = false;
    websocket = socket;
    // Set binary type to arraybuffer to handle raw binary data from the UART.
    socket.binaryType = "arraybuffer";

    const notifyClosed = (event) => {
        if (closeNotified) return;
        closeNotified = true;

        stopConnectionTimeout(socket);
        stopHeartbeat(socket);
        if (websocket === socket) websocket = undefined;

        socket.onopen = null;
        socket.onclose = null;
        socket.onmessage = null;
        socket.onerror = null;

        if (onClose) onClose(event);
    };

    const handleHeartbeatTimeout = () => {
        notifyClosed({
            type: 'heartbeat-timeout',
            wasClean: false,
            code: 1006,
            reason: 'Heartbeat timeout',
        });

        if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING) {
            socket.close();
        }
    };

    const handleConnectionTimeout = () => {
        notifyClosed({
            type: 'connection-timeout',
            wasClean: false,
            code: 1006,
            reason: 'Connection timeout',
        });

        if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING) {
            socket.close();
        }
    };

    // Assign event handlers, wrapping user-provided callbacks to include heartbeat logic
    socket.onopen = (event) => {
        if (websocket !== socket || closeNotified) return;
        stopConnectionTimeout(socket);
        console.log('WebSocket connection opened.');
        startHeartbeat(socket, handleHeartbeatTimeout);
        if (onOpen) {
            onOpen(event);
        }
    };

    socket.onclose = (event) => {
        console.log('WebSocket connection closed:', event);
        notifyClosed(event);
    };

    socket.onmessage = (event) => {
        if (websocket !== socket || closeNotified) return;

        if (event.data === 'pong') {
            if (heartbeatSocket === socket && pongTimeoutId) {
                clearTimeout(pongTimeoutId);
                pongTimeoutId = null;
            }
        } else {
            // If it's not a pong message, pass it to the user's onMessage callback
            if (onMessage) {
                onMessage(event);
            } else {
                console.log('WebSocket message received:', event.data);
            }
        }
    };

    socket.onerror = (error) => {
        if (websocket !== socket || closeNotified) return;
        console.error('WebSocket error:', error);
        if (onError) {
            onError(error);
        }
    };

    startConnectionTimeout(socket, handleConnectionTimeout);
}

/** Closes the status WebSocket without scheduling a reconnect in the caller. */
export function closeWebSocket() {
    const socket = websocket;
    websocket = undefined;
    stopConnectionTimeout(socket);
    stopHeartbeat(socket);
    if (!socket) return;

    socket.onopen = null;
    socket.onclose = null;
    socket.onmessage = null;
    socket.onerror = null;

    if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING) {
        socket.close();
    }
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
