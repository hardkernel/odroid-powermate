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
    addEventToTable,
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
let isRecording = false;
let recordedData = [];
let recordedEvents = [];

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

// User Settings DOM Elements
const userSettingsForm = document.getElementById('user-settings-form');
const newUsernameInput = document.getElementById('new-username');
const newPasswordInput = document.getElementById('new-password');
const confirmPasswordInput = document.getElementById('confirm-password');

// Metrics Tab DOM Elements
const recordButton = document.getElementById('record-button');
const stopButton = document.getElementById('stop-button');
const downloadCsvButton = document.getElementById('download-csv-button');
const downloadEventsCsvButton = document.getElementById('download-events-csv-button');


// --- WebSocket Event Handlers ---

function onWsOpen() {
    updateWebsocketStatus(true);
    console.log('Connected to WebSocket Server');
}

function onWsClose() {
    updateWebsocketStatus(false);
    console.warn('Connection closed. Reconnecting...');
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
                        timestamp: sensorData.timestampMs,
                        uptime: sensorData.uptimeMs
                    };
                    updateSensorUI(sensorPayload);

                    if (isRecording) {
                        recordedData.push(sensorPayload);
                    }

                    // Update uptime separately from the sensor data payload
                    if (sensorData.uptimeMs !== undefined) {
                        updateUptimeUI(sensorData.uptimeMs / 1000);
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

            case 'eventData':
                if (decodedMessage.eventData) {
                    const level = decodedMessage.eventData.level;
                    const msg = decodedMessage.eventData.message;
                    const timestampMs = decodedMessage.eventData.timestampMs;
                    const uptimeMs = decodedMessage.eventData.uptimeMs;

                    recordedEvents.push({ level, timestampMs, uptimeMs, message: msg });
                    addEventToTable(level, timestampMs, uptimeMs, msg);

                    const dateStr = timestampMs ? new Date(Number(timestampMs)).toLocaleString() : 'Unknown Time';
                    const uptimeStr = uptimeMs ? (Number(uptimeMs) / 1000).toFixed(0) : '0';

                    const prefix = `[PowerMate] (${dateStr}) (ut: ${uptimeStr}s)`;

                    switch (level) {
                        case 0: // EV_INFO
                            console.info(`${prefix} ${msg}`);
                            break;
                        case 1: // EV_WARNING
                            console.warn(`${prefix} ${msg}`);
                            break;
                        case 2: // EV_CRITICAL
                        case 3: // EV_FATAL
                            console.error(`${prefix} ${msg}`);
                            break;
                        default:
                            console.log(`${prefix} ${msg}`);
                    }
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

// --- User Settings Functions ---
async function handleUserSettingsSubmit(event) {
    event.preventDefault();

    const newUsername = newUsernameInput.value;
    const newPassword = newPasswordInput.value;
    const confirmPassword = confirmPasswordInput.value;

    if (!newUsername || !newPassword || !confirmPassword) {
        alert('Please fill in all fields for username and password.');
        return;
    }

    if (newPassword !== confirmPassword) {
        alert('New password and confirm password do not match.');
        return;
    }

    try {
        const response = await api.updateUserSettings(newUsername, newPassword);
        if (response && (response.status === 'ok' || response.auth_status === 'updated')) {
            alert('Username and password updated successfully. Please log in again with new credentials.');
            handleLogout(); // Force logout to re-authenticate with new credentials
        } else {
            alert(`Failed to update credentials: ${response.message || 'Unknown error'}`);
        }
    } catch (error) {
        console.error('Error updating user settings:', error);
        alert(`Error updating user settings: ${error.message}`);
    }
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

// --- Recording and Downloading Functions ---

function startRecording() {
    isRecording = true;
    recordedData = [];
    recordButton.style.display = 'none';
    stopButton.style.display = 'inline-block';
    downloadCsvButton.style.display = 'none';
    console.log('Recording started.');
}

function stopRecording() {
    isRecording = false;
    recordButton.style.display = 'inline-block';
    stopButton.style.display = 'none';
    if (recordedData.length > 0) {
        downloadCsvButton.style.display = 'inline-block';
    }
    console.log('Recording stopped. Data points captured:', recordedData.length);
}

function downloadCSV() {
    if (recordedData.length === 0) {
        alert('No data to download.');
        return;
    }

    const headers = [
        'timestamp', 'uptime_ms',
        'vin_voltage', 'vin_current', 'vin_power',
        'main_voltage', 'main_current', 'main_power',
        'usb_voltage', 'usb_current', 'usb_power'
    ];
    const csvRows = [headers.join(',')];

    recordedData.forEach(data => {
        const timestamp = new Date(data.timestamp).toISOString();
        const row = [
            timestamp,
            data.uptime,
            Number(data.VIN.voltage).toFixed(3), Number(data.VIN.current).toFixed(3), Number(data.VIN.power).toFixed(3),
            Number(data.MAIN.voltage).toFixed(3), Number(data.MAIN.current).toFixed(3), Number(data.MAIN.power).toFixed(3),
            Number(data.USB.voltage).toFixed(3), Number(data.USB.current).toFixed(3), Number(data.USB.power).toFixed(3)
        ];
        csvRows.push(row.join(','));
    });

    const blob = new Blob([csvRows.join('\n')], { type: 'text/csv;charset=utf-8;' });
    const link = document.createElement('a');
    const url = URL.createObjectURL(blob);
    
    const now = new Date();
    const pad = (num) => num.toString().padStart(2, '0');
    const datePart = `${now.getFullYear().toString().slice(-2)}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}`;
    const timePart = `${pad(now.getHours())}-${pad(now.getMinutes())}`;
    const filename = `powermate_${datePart}_${timePart}.csv`;

    link.setAttribute('href', url);
    link.setAttribute('download', filename);
    link.style.visibility = 'hidden';
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
}

function downloadEventsCSV() {
    if (recordedEvents.length === 0) {
        alert('No event data to download.');
        return;
    }

    const headers = ['timestamp', 'uptime_ms', 'level', 'message'];
    const csvRows = [headers.join(',')];

    recordedEvents.forEach(data => {
        const timestamp = data.timestampMs ? new Date(Number(data.timestampMs)).toISOString() : '';
        const uptime = data.uptimeMs ? data.uptimeMs : '';

        let levelText = '';
        switch (data.level) {
            case 0: levelText = 'INFO'; break;
            case 1: levelText = 'WARNING'; break;
            case 2: levelText = 'CRITICAL'; break;
            case 3: levelText = 'FATAL'; break;
            default: levelText = 'UNKNOWN';
        }

        const safeMsg = `"${data.message.replace(/"/g, '""')}"`;

        csvRows.push([timestamp, uptime, levelText, safeMsg].join(','));
    });

    const blob = new Blob([csvRows.join('\n')], { type: 'text/csv;charset=utf-8;' });
    const link = document.createElement('a');
    const url = URL.createObjectURL(blob);

    const now = new Date();
    const pad = (num) => num.toString().padStart(2, '0');
    const datePart = `${now.getFullYear().toString().slice(-2)}-${pad(now.getMonth() + 1)}-${pad(now.getDate())}`;
    const timePart = `${pad(now.getHours())}-${pad(now.getMinutes())}`;
    const filename = `powermate_events_${datePart}_${timePart}.csv`;

    link.setAttribute('href', url);
    link.setAttribute('download', filename);
    link.style.visibility = 'hidden';
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);
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
    
    // Attach listeners for recording/downloading
    recordButton.addEventListener('click', startRecording);
    stopButton.addEventListener('click', stopRecording);
    downloadCsvButton.addEventListener('click', downloadCSV);
    downloadEventsCsvButton.addEventListener('click', downloadEventsCSV);

    connect();

    // Attach user settings form listener
    if (userSettingsForm) {
        userSettingsForm.addEventListener('submit', handleUserSettingsSubmit);
    }
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
