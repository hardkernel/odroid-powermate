/**
 * @file main.js
 * @description The main entry point for the web application.
 * This file imports all necessary modules, sets up the application structure,
 * initializes components, and establishes the WebSocket connection.
 */

// --- Stylesheets ---
import 'bootstrap/dist/css/bootstrap.min.css';
import 'bootstrap-icons/font/bootstrap-icons.css';
import './style.css';

// --- Module Imports -- -
import {StatusMessage} from './proto.js';
import * as api from './api.js';
import {initWebSocket} from './websocket.js';
import {setupTerminal, term} from './terminal.js';
import {
    applyTheme,
    initUI,
    updateControlStatus,
    updateSensorUI,
    updateSwitchStatusUI,
    updateUptimeUI,
    updateVersionUI,
    updateWebsocketStatus,
    updateWifiStatusUI
} from './ui.js';
import {setupEventListeners} from './events.js';

// --- Globals ---
// StatusMessage is imported directly from the generated proto.js file.

// --- WebSocket Event Handlers ---

function onWsOpen() {
    updateWebsocketStatus(true);
    if (term) {
        term.write('\x1b[32mConnected to WebSocket Server\x1b[0m\r\n');
    }
}

function onWsClose() {
    updateWebsocketStatus(false);
    if (term) {
        term.write('\r\n\x1b[31mConnection closed. Reconnecting...\x1b[0m\r\n');
    }
    setTimeout(connect, 2000);
}

/**
 * Callback for when a message is received from the WebSocket server.
 * @param {MessageEvent} event - The WebSocket message event.
 */
function onWsMessage(event) {
    if (!(event.data instanceof ArrayBuffer)) {
        console.warn('Message is not an ArrayBuffer, skipping protobuf decoding.');
        return;
    }

    const buffer = new Uint8Array(event.data);
    try {
        const decodedMessage = StatusMessage.decode(buffer);
        const payloadType = decodedMessage.payload;

        switch (payloadType) {
            case 'sensorData': {
                const sensorData = decodedMessage.sensorData;
                if (sensorData) {
                    // Create a payload for the sensor UI (charts and header)
                    const sensorPayload = {
                        USB: sensorData.usb,
                        MAIN: sensorData.main,
                        VIN: sensorData.vin,
                        timestamp: sensorData.timestamp
                    };
                    updateSensorUI(sensorPayload);

                    // Update uptime separately from the sensor data payload
                    if (sensorData.uptimeSec !== undefined) {
                        updateUptimeUI(sensorData.uptimeSec);
                    }
                }
                break;
            }

            case 'wifiStatus':
                updateWifiStatusUI(decodedMessage.wifiStatus);
                break;

            case 'swStatus':
                if (decodedMessage.swStatus) {
                    updateSwitchStatusUI(decodedMessage.swStatus);
                }
                break;

            case 'uartData':
                if (term && decodedMessage.uartData && decodedMessage.uartData.data) {
                    term.write(decodedMessage.uartData.data);
                }
                break;

            default:
                if (payloadType !== undefined) {
                    console.warn('Received message with unknown or empty payload type:', payloadType);
                }
                break;
        }
    } catch (e) {
        console.error('Error decoding protobuf message:', e);
    }
}


// --- Application Initialization ---

async function initializeVersion() {
    try {
        const versionData = await api.fetchVersion();
        if (versionData && versionData.version) {
            updateVersionUI(versionData.version);
        }
    } catch (error) {
        console.error('Error fetching version:', error);
        updateVersionUI('N/A');
    }
}

function connect() {
    updateControlStatus();
    initWebSocket({ onOpen: onWsOpen, onClose: onWsClose, onMessage: onWsMessage });
}

function initialize() {
    initUI();
    setupTerminal();
    initializeVersion();

    const savedTheme = localStorage.getItem('theme') || (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
    applyTheme(savedTheme);

    setupEventListeners();

    connect();
}

// --- Start Application ---
document.addEventListener('DOMContentLoaded', initialize);
