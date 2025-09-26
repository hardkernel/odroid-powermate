/**
 * @file websocket.js
 * @description This module handles the WebSocket connection for real-time, two-way
 * communication with the server. It provides functions to initialize the connection
 * and send messages.
 */

// The WebSocket instance, exported for potential direct access if needed.
export let websocket;

// The WebSocket server address, derived from the current page's host (hostname + port).
const baseGateway = `ws://${window.location.host}/ws`;

/**
 * Initializes the WebSocket connection and sets up event handlers.
 * @param {Object} callbacks - An object containing callback functions for WebSocket events.
 * @param {function} callbacks.onOpen - Called when the connection is successfully opened.
 * @param {function} callbacks.onClose - Called when the connection is closed.
 * @param {function} callbacks.onMessage - Called when a message is received from the server.
 */
export function initWebSocket({onOpen, onClose, onMessage}) {
    const token = localStorage.getItem('authToken');
    let gateway = baseGateway;

    if (token) {
        gateway = `${baseGateway}?token=${token}`;
    }

    console.log(`Trying to open a WebSocket connection to ${gateway}...`);
    websocket = new WebSocket(gateway);
    // Set binary type to arraybuffer to handle raw binary data from the UART.
    websocket.binaryType = "arraybuffer";

    // Assign event handlers from the provided callbacks
    if (onOpen) websocket.onopen = onOpen;
    if (onClose) websocket.onclose = onClose;
    if (onMessage) websocket.onmessage = onMessage;
}

/**
 * Sends data over the WebSocket connection if it is open.
 * @param {string | ArrayBuffer} data - The data to send to the server.
 */
export function sendWebsocketMessage(data) {
    if (websocket && websocket.readyState === WebSocket.OPEN) {
        websocket.send(data);
    }
}
