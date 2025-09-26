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

// --- DOM Elements ---
const loginContainer = document.getElementById('login-container');
const mainContent = document.querySelector('main.container');
const loginForm = document.getElementById('login-form');
const usernameInput = document.getElementById('username');
const passwordInput = document.getElementById('password');
const loginAlert = document.getElementById('login-alert');
const logoutButton = document.getElementById('logout-button');
const themeToggleLogin = document.getElementById('theme-toggle-login');
const themeIconLogin = document.getElementById('theme-icon-login');
const themeToggleMain = document.getElementById('theme-toggle');
const themeIconMain = document.getElementById('theme-icon');


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

// --- Authentication Functions ---

function checkAuth() {
    const token = localStorage.getItem('authToken');
    if (token) {
        return true;
    } else {
        return false;
    }
}

async function handleLogin(event) {
    event.preventDefault();
    const username = usernameInput.value;
    const password = passwordInput.value;

    try {
        const response = await api.login(username, password);
        if (response && response.token) {
            localStorage.setItem('authToken', response.token);
            loginAlert.classList.add('d-none');
            loginContainer.style.setProperty('display', 'none', 'important');
            initializeMainAppContent(); // After successful login, initialize the main app
        } else {
            loginAlert.textContent = 'Login failed: No token received.';
            loginAlert.classList.remove('d-none');
        }
    } catch (error) {
        console.error('Login error:', error);
        loginAlert.textContent = `Login failed: ${error.message}`;
        loginAlert.classList.remove('d-none');
    }
}

function handleLogout() {
    localStorage.removeItem('authToken');
    // Hide main content and show login form
    loginContainer.style.setProperty('display', 'flex', 'important');
    mainContent.style.setProperty('display', 'none', 'important');
    // Optionally, disconnect WebSocket or perform other cleanup
    // For now, just hide the main content.
}

// --- Theme Toggle Functions ---
function setupThemeToggles() {
    // Initialize theme for login page
    const savedTheme = localStorage.getItem('theme') || (window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light');
    applyTheme(savedTheme);
    themeToggleLogin.checked = savedTheme === 'dark';
    themeIconLogin.className = savedTheme === 'dark' ? 'bi bi-moon-stars-fill' : 'bi bi-sun-fill';

    // Sync main theme toggle with login theme toggle (initial state)
    themeToggleMain.checked = savedTheme === 'dark';
    themeIconMain.className = savedTheme === 'dark' ? 'bi bi-moon-stars-fill' : 'bi bi-sun-fill';

    themeToggleLogin.addEventListener('change', () => {
        const newTheme = themeToggleLogin.checked ? 'dark' : 'light';
        applyTheme(newTheme);
        localStorage.setItem('theme', newTheme);
        themeIconLogin.className = newTheme === 'dark' ? 'bi bi-moon-stars-fill' : 'bi bi-sun-fill';
        themeToggleMain.checked = themeToggleLogin.checked; // Keep main toggle in sync
        themeIconMain.className = themeIconLogin.className; // Keep main icon in sync
    });

    themeToggleMain.addEventListener('change', () => {
        const newTheme = themeToggleMain.checked ? 'dark' : 'light';
        applyTheme(newTheme);
        localStorage.setItem('theme', newTheme);
        themeIconMain.className = newTheme === 'dark' ? 'bi bi-moon-stars-fill' : 'bi bi-sun-fill';
        themeToggleLogin.checked = themeToggleMain.checked; // Keep login toggle in sync
        themeIconLogin.className = themeIconMain.className; // Keep login icon in sync
    });
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

// New function to initialize main app content after successful login or on initial load if authenticated
function initializeMainAppContent() {
    loginContainer.style.setProperty('display', 'none', 'important');
    mainContent.style.setProperty('display', 'block', 'important');

    initUI();
    setupTerminal();
    initializeVersion();
    setupEventListeners(); // Attach main app event listeners
    logoutButton.addEventListener('click', handleLogout); // Attach logout listener
    connect();
}

function initialize() {
    setupThemeToggles(); // Setup theme toggles for both login and main (initial sync)

    // Always attach login form listener
    loginForm.addEventListener('submit', handleLogin);

    if (!checkAuth()) { // If NOT authenticated
        // Show login form
        loginContainer.style.setProperty('display', 'flex', 'important');
        mainContent.style.setProperty('display', 'none', 'important');
        console.log('Not authenticated. Login form displayed. Main app content NOT initialized.');
        return; // IMPORTANT: Stop execution here if not authenticated
    }

    // If authenticated, initialize main content
    console.log('Authenticated. Initializing main app content.');
    initializeMainAppContent();
}

// --- Start Application ---
document.addEventListener('DOMContentLoaded', initialize);
