/**
 * @file events.js
 * @description This module sets up all the event listeners for the application.
 * It connects user interactions (like clicks and toggles) to the corresponding
 * functions in other modules (UI, API, etc.).
 */

import * as dom from './dom.js';
import * as api from './api.js';
import {getAuthHeaders, handleResponse} from './api.js'; // Import auth functions
import * as ui from './ui.js';
import {clearTerminal, downloadTerminalOutput, fitTerminal} from './terminal.js';
import {debounce, isMobile} from './utils.js';

// A flag to track if charts have been initialized
let chartsInitialized = false;
let listenersAttached = false;

// --- Helper functions for settings ---

function updateSliderValue(slider, span) {
    if (!slider || !span) return;
    let value = parseFloat(slider.value).toFixed(1);
    if (value <= 0) {
        span.textContent = 'Disabled';
    } else {
        span.textContent = `${value} A`;
    }
}

function loadCurrentLimitSettings() {
    fetch('/api/setting', {
        headers: getAuthHeaders(), // Add auth headers
    })
        .then(handleResponse) // Handle response for 401
        .then(response => response.json())
        .then(data => {
            if (data.vin_current_limit !== undefined) {
                dom.vinSlider.value = data.vin_current_limit;
                updateSliderValue(dom.vinSlider, dom.vinValueSpan);
            }
            if (data.main_current_limit !== undefined) {
                dom.mainSlider.value = data.main_current_limit;
                updateSliderValue(dom.mainSlider, dom.mainValueSpan);
            }
            if (data.usb_current_limit !== undefined) {
                dom.usbSlider.value = data.usb_current_limit;
                updateSliderValue(dom.usbSlider, dom.usbValueSpan);
            }
        })
        .catch(error => console.error('Error fetching current limit settings:', error));
}

/**
 * Sets up all event listeners for the application's interactive elements.
 * This function is now idempotent and will only attach listeners once.
 */
export function setupEventListeners() {
    if (listenersAttached) {
        console.log("Event listeners already attached. Skipping.");
        return;
    }

    // --- Terminal Controls ---
    dom.clearButton.addEventListener('click', clearTerminal);
    dom.downloadButton.addEventListener('click', downloadTerminalOutput);

    // --- Power Controls ---
    dom.mainPowerToggle.addEventListener('change', () => api.postControlCommand({'load_12v_on': dom.mainPowerToggle.checked}).then(ui.updateControlStatus));
    dom.usbPowerToggle.addEventListener('change', () => api.postControlCommand({'load_5v_on': dom.usbPowerToggle.checked}).then(ui.updateControlStatus));
    dom.resetButton.addEventListener('click', () => api.postControlCommand({'reset_trigger': true}));
    dom.powerActionButton.addEventListener('click', () => api.postControlCommand({'power_trigger': true}));

    // --- Settings Modal Controls ---
    dom.scanWifiButton.addEventListener('click', ui.scanForWifi);
    dom.wifiConnectButton.addEventListener('click', ui.connectToWifi);
    dom.networkApplyButton.addEventListener('click', ui.applyNetworkSettings);
    dom.apModeApplyButton.addEventListener('click', ui.applyApModeSettings);
    dom.baudRateApplyButton.addEventListener('click', ui.applyBaudRateSettings);

    // --- Device Settings (Reboot) ---
    if (dom.rebootButton) {
        dom.rebootButton.addEventListener('click', () => {
            if (confirm('Are you sure you want to reboot the device?')) {
                fetch('/api/reboot', {
                    method: 'POST',
                    headers: getAuthHeaders(), // Add auth headers
                })
                    .then(handleResponse) // Handle response for 401
                    .then(response => response.json())
                    .then(data => {
                        console.log('Reboot command sent:', data);
                        ui.hideSettingsModal();
                        alert('Reboot command sent. The device will restart in 3 seconds.');
                    })
                    .catch(error => {
                        console.error('Error sending reboot command:', error);
                        alert('Failed to send reboot command.');
                    });
            }
        });
    }

    // --- Current Limit Settings ---
    dom.vinSlider.addEventListener('input', () => updateSliderValue(dom.vinSlider, dom.vinValueSpan));
    dom.mainSlider.addEventListener('input', () => updateSliderValue(dom.mainSlider, dom.mainValueSpan));
    dom.usbSlider.addEventListener('input', () => updateSliderValue(dom.usbSlider, dom.usbValueSpan));

    dom.currentLimitApplyButton.addEventListener('click', () => {
        const settings = {
            vin_current_limit: parseFloat(dom.vinSlider.value),
            main_current_limit: parseFloat(dom.mainSlider.value),
            usb_current_limit: parseFloat(dom.usbSlider.value)
        };

        fetch('/api/setting', {
            method: 'POST',
            headers: {
                'Content-Type': 'application/json',
                ...getAuthHeaders(), // Add auth headers
            },
            body: JSON.stringify(settings),
        })
            .then(handleResponse) // Handle response for 401
            .then(response => response.json())
            .then(data => {
                console.log('Current limit settings applied:', data);
            })
            .catch((error) => {
                console.error('Error applying current limit settings:', error);
                alert('Failed to apply current limit settings.');
            });
    });


    // --- Settings Modal Toggles (for showing/hiding sections) ---
    dom.apModeToggle.addEventListener('change', () => {
        dom.apModeConfig.style.display = dom.apModeToggle.checked ? 'block' : 'none';
    });

    dom.staticIpToggle.addEventListener('change', () => {
        dom.staticIpConfig.style.display = dom.staticIpToggle.checked ? 'block' : 'none';
    });

    // --- General App Listeners ---
    dom.settingsButton.addEventListener('click', ui.initializeSettings);

    // --- Accessibility & Modal Events ---
    dom.settingsModal.addEventListener('show.bs.modal', () => {
        // Load settings when the modal is about to be shown
        loadCurrentLimitSettings();
    });

    const blurActiveElement = () => {
        if (document.activeElement && typeof document.activeElement.blur === 'function') {
            document.activeElement.blur();
        }
    };
    dom.settingsModal.addEventListener('hide.bs.modal', blurActiveElement);
    dom.wifiModalEl.addEventListener('hide.bs.modal', blurActiveElement);

    // --- Bootstrap Tab Events ---
    document.querySelectorAll('button[data-bs-toggle="tab"]').forEach(tabEl => {
        tabEl.addEventListener('shown.bs.tab', async (event) => {
            const tabId = event.target.getAttribute('data-bs-target');

            if (tabId === '#graph-tab-pane') {
                // Dynamically import the chart module only when the tab is shown
                const chartModule = await import('./chart.js');

                if (!chartsInitialized) {
                    chartModule.initCharts();
                    chartsInitialized = true;
                } else {
                    chartModule.resizeCharts();
                }
            } else if (tabId === '#terminal-tab-pane') {
                // Fit the terminal when its tab is shown, especially for mobile.
                if (isMobile()) {
                    fitTerminal();
                }
            }
        });
    });

    // --- Window Resize Event ---
    // Debounced to avoid excessive calls during resizing.
    window.addEventListener('resize', debounce(ui.handleResize, 150));

    listenersAttached = true;
}
