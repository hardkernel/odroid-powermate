/**
 * @file ui.js
 * @description This module manages all UI interactions and updates.
 * It bridges the gap between backend data (from API and WebSockets) and the user-facing components,
 * handling everything from theme changes to dynamic content updates.
 */

import * as bootstrap from 'bootstrap';
import * as dom from './dom.js';
import * as api from './api.js';
import {formatUptime, isMobile} from './utils.js';
import {applyTerminalTheme, fitTerminal} from './terminal.js';
import {applyChartsTheme, resizeCharts, updateCharts} from './chart.js';

// Instance of the Bootstrap Modal for Wi-Fi connection
let wifiModal;

/**
 * Initializes the UI components, such as the Bootstrap modal.
 */
export function initUI() {
    wifiModal = new bootstrap.Modal(dom.wifiModalEl);
}

/**
 * Applies the selected theme (light or dark) to the entire application.
 * @param {string} themeName - The name of the theme ('light' or 'dark').
 */
export function applyTheme(themeName) {
    const isDark = themeName === 'dark';
    dom.htmlEl.setAttribute('data-bs-theme', themeName);
    dom.themeIcon.className = isDark ? 'bi bi-moon-stars-fill' : 'bi bi-sun-fill';
    dom.themeToggle.checked = isDark;
    applyTerminalTheme(themeName);
    applyChartsTheme(themeName);
}

/**
 * Updates the UI with the latest sensor data.
 * @param {Object} data - The sensor data object from the WebSocket.
 */
export function updateSensorUI(data) {
    // Display VIN channel data in the header as a primary overview
    if (data.VIN) {
        dom.voltageDisplay.textContent = `${data.VIN.voltage.toFixed(2)} V`;
        dom.currentDisplay.textContent = `${data.VIN.current.toFixed(2)} A`;
        dom.powerDisplay.textContent = `${data.VIN.power.toFixed(2)} W`;
    }

    // Pass the entire multi-channel data object to the charts
    updateCharts(data);
}

/**
 * Updates the system uptime display in the UI.
 * @param {number} uptimeInSeconds - The system uptime in seconds.
 */
export function updateUptimeUI(uptimeInSeconds) {
    if (uptimeInSeconds !== undefined) {
        dom.uptimeDisplay.textContent = formatUptime(uptimeInSeconds);
    }
}

/**
 * Updates the power switch toggle states based on WebSocket data.
 * @param {Object} swStatus - The switch status object from the WebSocket message.
 */
export function updateSwitchStatusUI(swStatus) {
    if (swStatus) {
        if (swStatus.main !== undefined) {
            dom.mainPowerToggle.checked = swStatus.main;
        }
        if (swStatus.usb !== undefined) {
            dom.usbPowerToggle.checked = swStatus.usb;
        }
    }
}

/**
 * Updates the Wi-Fi status indicator in the header.
 * @param {Object} data - The Wi-Fi status object from the WebSocket.
 */
export function updateWifiStatusUI(data) {
    if (data.connected) {
        // Update header status
        dom.wifiSsidStatus.textContent = data.ssid;
        dom.wifiStatus.title = `Signal Strength: ${data.rssi} dBm`;
        let iconClass = 'bi me-2 ';
        if (data.rssi >= -60) iconClass += 'bi-wifi';
        else if (data.rssi >= -75) iconClass += 'bi-wifi-2';
        else iconClass += 'bi-wifi-1';
        dom.wifiIcon.className = iconClass;
        dom.wifiStatus.classList.replace('text-muted', 'text-success');
        dom.wifiStatus.classList.remove('text-danger');

        // Update settings modal
        dom.currentWifiSsid.textContent = data.ssid;
        dom.currentWifiIp.textContent = `IP Address: ${data.ipAddress || 'N/A'}`;
    } else {
        // Update header status
        dom.wifiSsidStatus.textContent = 'Disconnected';
        dom.wifiStatus.title = '';
        dom.wifiIcon.className = 'bi bi-wifi-off me-2';
        dom.wifiStatus.classList.replace('text-success', 'text-muted');
        dom.wifiStatus.classList.remove('text-danger');

        // Update settings modal
        dom.currentWifiSsid.textContent = 'Not Connected';
        dom.currentWifiIp.textContent = 'IP Address: -';
    }
}

/**
 * Updates the version information in the footer.
 * @param {string} version - The firmware version string.
 */
export function updateVersionUI(version) {
    if (version) {
        dom.versionInfo.textContent = `${version}`;
    }
}

/**
 * Initiates a Wi-Fi scan and updates the settings modal with the results.
 */
export async function scanForWifi() {
    dom.scanWifiButton.disabled = true;
    dom.scanWifiButton.innerHTML = `<span class="spinner-border spinner-border-sm" aria-hidden="true"></span> Scanning...`;
    dom.wifiApList.innerHTML = '<tr><td colspan="3" class="text-center">Scanning for networks...</td></tr>';

    try {
        const apRecords = await api.fetchWifiScan();
        dom.wifiApList.innerHTML = ''; // Clear loading message

        if (apRecords.length === 0) {
            dom.wifiApList.innerHTML = '<tr><td colspan="3" class="text-center">No networks found.</td></tr>';
        } else {
            apRecords.forEach(ap => {
                const row = document.createElement('tr');
                row.className = 'wifi-ap-row';

                let rssiIcon;
                if (ap.rssi >= -60) rssiIcon = 'bi-wifi';
                else if (ap.rssi >= -75) rssiIcon = 'bi-wifi-2';
                else rssiIcon = 'bi-wifi-1';

                row.innerHTML = `
                    <td>${ap.ssid}</td>
                    <td class="text-center"><i class="bi ${rssiIcon}"></i></td>
                    <td>${ap.authmode}</td>
                `;

                row.addEventListener('click', () => {
                    dom.wifiSsidConnectInput.value = ap.ssid;
                    dom.wifiPasswordConnectInput.value = '';
                    wifiModal.show();
                    dom.wifiModalEl.addEventListener('shown.bs.modal', () => {
                        dom.wifiPasswordConnectInput.focus();
                    }, {once: true});
                });

                dom.wifiApList.appendChild(row);
            });
        }
    } catch (error) {
        console.error('Error scanning for Wi-Fi:', error);
        dom.wifiApList.innerHTML = `<tr><td colspan="3" class="text-center text-danger">Scan failed: ${error.message}</td></tr>`;
    } finally {
        dom.scanWifiButton.disabled = false;
        dom.scanWifiButton.innerHTML = 'Scan';
    }
}

/**
 * Handles the Wi-Fi connection process, sending credentials to the server.
 */
export async function connectToWifi() {
    const ssid = dom.wifiSsidConnectInput.value;
    const password = dom.wifiPasswordConnectInput.value;
    if (!ssid) return;

    dom.wifiConnectButton.disabled = true;
    dom.wifiConnectButton.innerHTML = `<span class="spinner-border spinner-border-sm" aria-hidden="true"></span> Connecting...`;

    try {
        const result = await api.postWifiConnect(ssid, password);
        if (result.status === 'connection_initiated') {
            wifiModal.hide();
            setTimeout(() => {
                alert(`Connection to "${ssid}" initiated. The device will try to reconnect. Please check the Wi-Fi status icon.`);
            }, 500);
        } else {
            throw new Error(result.message || 'Unknown server response.');
        }
    } catch (error) {
        console.error('Error connecting to Wi-Fi:', error);
        alert(`Failed to connect: ${error.message}`);
    } finally {
        dom.wifiConnectButton.disabled = false;
        dom.wifiConnectButton.innerHTML = 'Connect';
    }
}

/**
 * Applies network settings (Static IP or DHCP) by sending the configuration to the server.
 */
export async function applyNetworkSettings() {
    const useStatic = dom.staticIpToggle.checked;
    let payload;

    dom.networkApplyButton.disabled = true;
    dom.networkApplyButton.innerHTML = `<span class="spinner-border spinner-border-sm" aria-hidden="true"></span> Applying...`;

    if (useStatic) {
        const ip = dom.staticIpInput.value;
        const gateway = dom.staticGatewayInput.value;
        const subnet = dom.staticNetmaskInput.value;
        const dns1 = dom.dns1Input.value;
        const dns2 = dom.dns2Input.value;

        if (!ip || !gateway || !subnet || !dns1) {
            alert('For static IP, you must provide IP Address, Gateway, Netmask, and DNS Server.');
            dom.networkApplyButton.disabled = false;
            dom.networkApplyButton.innerHTML = 'Apply';
            return;
        }

        payload = {net_type: 'static', ip, gateway, subnet, dns1};
        if (dns2) payload.dns2 = dns2;
    } else {
        payload = {net_type: 'dhcp'};
    }

    try {
        await api.postNetworkSettings(payload);
        alert('Network settings applied. Reconnect to the network for changes to take effect.');
        initializeSettings();
    } catch (error) {
        console.error('Error applying network settings:', error);
        alert(`Failed to apply settings: ${error.message}`);
    } finally {
        dom.networkApplyButton.disabled = false;
        dom.networkApplyButton.innerHTML = 'Apply';
    }
}

/**
 * Applies AP Mode settings (AP+STA or STA) by sending the configuration to the server.
 */
export async function applyApModeSettings() {
    const mode = dom.apModeToggle.checked ? 'apsta' : 'sta';
    let payload = {mode};

    dom.apModeApplyButton.disabled = true;
    dom.apModeApplyButton.innerHTML = `<span class="spinner-border spinner-border-sm" aria-hidden="true"></span> Applying...`;

    if (mode === 'apsta') {
        const ap_ssid = dom.apSsidInput.value;
        const ap_password = dom.apPasswordInput.value;

        if (!ap_ssid) {
            alert('AP SSID cannot be empty when enabling APSTA mode.');
            dom.apModeApplyButton.disabled = false;
            dom.apModeApplyButton.innerHTML = 'Apply';
            return;
        }
        payload.ap_ssid = ap_ssid;
        if (ap_password) {
            payload.ap_password = ap_password;
        }
    }

    try {
        await api.postNetworkSettings(payload); // Reuses the same API endpoint
        alert(`Successfully switched mode to ${mode}. The device will now reconfigure.`);
        initializeSettings();
    } catch (error) {
        console.error('Error switching Wi-Fi mode:', error);
        alert(`Failed to switch mode: ${error.message}`);
    } finally {
        dom.apModeApplyButton.disabled = false;
        dom.apModeApplyButton.innerHTML = 'Apply';
    }
}

/**
 * Applies the selected UART baud rate by sending it to the server.
 */
export async function applyBaudRateSettings() {
    const baudrate = dom.baudRateSelect.value;
    dom.baudRateApplyButton.disabled = true;
    dom.baudRateApplyButton.innerHTML = `<span class="spinner-border spinner-border-sm" aria-hidden="true"></span> Applying...`;

    try {
        await api.postBaudRateSetting(baudrate);
    } catch (error) {
        console.error('Error applying baud rate:', error);
    } finally {
        dom.baudRateApplyButton.disabled = false;
        dom.baudRateApplyButton.innerHTML = 'Apply';
    }
}

/**
 * Fetches and displays the current network and device settings in the settings modal.
 */
export async function initializeSettings() {
    try {
        const data = await api.fetchSettings();

        // Wi-Fi Connection Status
        if (data.connected) {
            dom.currentWifiSsid.textContent = data.ssid;
            dom.currentWifiIp.textContent = `IP Address: ${data.ip ? data.ip.ip : 'N/A'}`;
        } else {
            dom.currentWifiSsid.textContent = 'Not Connected';
            dom.currentWifiIp.textContent = 'IP Address: -';
        }

        // Network (Static/DHCP) Settings
        if (data.ip) {
            dom.staticIpInput.value = data.ip.ip || '';
            dom.staticGatewayInput.value = data.ip.gateway || '';
            dom.staticNetmaskInput.value = data.ip.subnet || '';
            dom.dns1Input.value = data.ip.dns1 || '';
            dom.dns2Input.value = data.ip.dns2 || '';
        }
        dom.staticIpToggle.checked = data.net_type === 'static';
        dom.staticIpConfig.style.display = dom.staticIpToggle.checked ? 'block' : 'none';

        // AP Mode Settings
        dom.apModeToggle.checked = data.mode === 'apsta';
        dom.apModeConfig.style.display = dom.apModeToggle.checked ? 'block' : 'none';
        dom.apSsidInput.value = ''; // For security, don't pre-fill
        dom.apPasswordInput.value = '';

        // Device Settings
        if (data.baudrate) {
            dom.baudRateSelect.value = data.baudrate;
        }

    } catch (error) {
        console.error('Error initializing settings:', error);
        // Reset fields on error
        dom.currentWifiSsid.textContent = 'Status Unknown';
        dom.currentWifiIp.textContent = 'IP Address: -';
        dom.staticIpToggle.checked = false;
        dom.staticIpConfig.style.display = 'none';
        dom.apModeToggle.checked = false;
        dom.apModeConfig.style.display = 'none';
    }
}

/**
 * Fetches and updates the status of the power control toggles.
 */
export async function updateControlStatus() {
    try {
        const status = await api.fetchControlStatus();
        dom.mainPowerToggle.checked = status.load_12v_on;
        dom.usbPowerToggle.checked = status.load_5v_on;
    } catch (error) {
        console.error('Error fetching control status:', error);
    }
}

/**
 * Handles window resize events to make components responsive.
 */
export function handleResize() {
    if (isMobile()) {
        fitTerminal();
    }
    resizeCharts();
}

/**
 * Updates the WebSocket connection status indicator in the header.
 * @param {boolean} isConnected - True if the WebSocket is connected, false otherwise.
 */
export function updateWebsocketStatus(isConnected) {
    if (isConnected) {
        dom.websocketStatusText.textContent = 'Online';
        dom.websocketIcon.className = 'bi bi-check-circle-fill me-2';
        dom.websocketStatus.classList.remove('text-danger');
        dom.websocketStatus.classList.add('text-success');
    } else {
        dom.websocketStatusText.textContent = 'Offline';
        dom.websocketIcon.className = 'bi bi-x-circle-fill me-2';
        dom.websocketStatus.classList.remove('text-success');
        dom.websocketStatus.classList.add('text-danger');
    }
}
