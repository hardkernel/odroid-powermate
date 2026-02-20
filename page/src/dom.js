/**
 * @file dom.js
 * @description This module selects and exports all necessary DOM elements for the application.
 * This centralizes DOM access, making it easier to manage and modify element selectors.
 */

// --- Theme Elements ---
export const themeToggle = document.getElementById('theme-toggle');
export const themeIcon = document.getElementById('theme-icon');
export const htmlEl = document.documentElement;

// --- Control & Status Elements ---
export const mainPowerToggle = document.getElementById('main-power-toggle');
export const usbPowerToggle = document.getElementById('usb-power-toggle');
export const resetButton = document.getElementById('reset-button');
export const powerActionButton = document.getElementById('power-action-button');
export const voltageDisplay = document.getElementById('voltage-display');
export const currentDisplay = document.getElementById('current-display');
export const powerDisplay = document.getElementById('power-display');
export const uptimeDisplay = document.getElementById('uptime-display');

// --- Terminal Elements ---
export const terminalContainer = document.getElementById('terminal');
export const clearButton = document.getElementById('clear-button');
export const downloadButton = document.getElementById('download-button');

// --- Chart & Graph Elements ---
export const graphTabPane = document.getElementById('graph-tab-pane');
export const powerChartCtx = document.getElementById('powerChart')?.getContext('2d');
export const voltageChartCtx = document.getElementById('voltageChart')?.getContext('2d');
export const currentChartCtx = document.getElementById('currentChart')?.getContext('2d');

// --- Event Table Elements ---
export const eventTableBody = document.getElementById('event-table-body');


// --- WebSocket Status Elements ---
export const websocketStatus = document.getElementById('websocket-status');
export const websocketIcon = document.getElementById('websocket-icon');
export const websocketStatusText = document.getElementById('websocket-status-text');

// --- Header Wi-Fi Status ---
export const wifiStatus = document.getElementById('wifi-status');
export const wifiIcon = document.getElementById('wifi-icon');
export const wifiSsidStatus = document.getElementById('wifi-ssid-status');

// --- Settings Modal & General ---
export const settingsButton = document.getElementById('settings-button');
export const settingsModal = document.getElementById('settingsModal');

// --- Wi-Fi Settings Elements ---
export const wifiApList = document.getElementById('wifi-ap-list');
export const scanWifiButton = document.getElementById('scan-wifi-button');
export const currentWifiSsid = document.getElementById('current-wifi-ssid');
export const currentWifiIp = document.getElementById('current-wifi-ip');

// --- Wi-Fi Connection Modal Elements ---
export const wifiModalEl = document.getElementById('wifiModal');
export const wifiSsidConnectInput = document.getElementById('wifi-ssid-connect');
export const wifiPasswordConnectInput = document.getElementById('wifi-password-connect');
export const wifiConnectButton = document.getElementById('wifi-connect-button');

// --- Static IP Config Elements ---
export const staticIpToggle = document.getElementById('static-ip-toggle');
export const staticIpConfig = document.getElementById('static-ip-config');
export const networkApplyButton = document.getElementById('network-apply-button');
export const staticIpInput = document.getElementById('ip-address');
export const staticGatewayInput = document.getElementById('gateway');
export const staticNetmaskInput = document.getElementById('netmask');
export const dns1Input = document.getElementById('dns1');
export const dns2Input = document.getElementById('dns2');

// --- AP Mode Config Elements ---
export const apModeToggle = document.getElementById('ap-mode-toggle');
export const apModeConfig = document.getElementById('ap-mode-config');
export const apModeApplyButton = document.getElementById('ap-mode-apply-button');
export const apSsidInput = document.getElementById('ap-ssid');
export const apPasswordInput = document.getElementById('ap-password');

// --- Device Settings Elements ---
export const baudRateSelect = document.getElementById('baud-rate-select');
export const baudRateApplyButton = document.getElementById('baud-rate-apply-button');
export const periodSlider = document.getElementById('period-slider');
export const periodValue = document.getElementById('period-value');
export const periodApplyButton = document.getElementById('period-apply-button');
export const rebootButton = document.getElementById('reboot-button');

// --- Current Limit Settings Elements ---
export const vinSlider = document.getElementById('vin-current-limit-slider');
export const vinValueSpan = document.getElementById('vin-current-limit-value');
export const mainSlider = document.getElementById('main-current-limit-slider');
export const mainValueSpan = document.getElementById('main-current-limit-value');
export const usbSlider = document.getElementById('usb-current-limit-slider');
export const usbValueSpan = document.getElementById('usb-current-limit-value');
export const currentLimitApplyButton = document.getElementById('current-limit-apply-button');

// --- Footer ---
export const versionInfo = document.getElementById('version-info');
