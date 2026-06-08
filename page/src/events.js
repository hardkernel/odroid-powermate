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
let powerControlRequestInFlight = false;
const CURRENT_LIMIT_STEP_A = 0.1;

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

function roundToStep(value) {
    return Math.round(value / CURRENT_LIMIT_STEP_A) * CURRENT_LIMIT_STEP_A;
}

function sliderMax(slider) {
    return parseFloat(slider.dataset.max || slider.max);
}

function setSliderValue(slider, value) {
    slider.value = roundToStep(value).toFixed(1);
}

function syncCurrentLimitPair(limitSlider, limitSpan, criticalSlider, criticalSpan, changedSlider = null) {
    if (!limitSlider || !criticalSlider) return;

    const limitCeiling = sliderMax(limitSlider);
    const criticalFloor = parseFloat(criticalSlider.min);
    const criticalCeiling = parseFloat(criticalSlider.max);
    let limit = parseFloat(limitSlider.value);
    let critical = parseFloat(criticalSlider.value);

    if (critical < criticalFloor) {
        critical = criticalFloor;
    }
    if (critical > criticalCeiling) {
        critical = criticalCeiling;
    }

    if (changedSlider === limitSlider && limit > 0 && limit >= critical) {
        const raisedCritical = Math.min(criticalCeiling, limit + CURRENT_LIMIT_STEP_A);
        if (raisedCritical > limit) {
            critical = raisedCritical;
        } else {
            limit = Math.max(0, critical - CURRENT_LIMIT_STEP_A);
        }
    }

    if (changedSlider === criticalSlider && limit > 0 && limit >= critical) {
        limit = Math.max(0, critical - CURRENT_LIMIT_STEP_A);
    }

    const effectiveLimitMax = Math.max(0, Math.min(limitCeiling, critical - CURRENT_LIMIT_STEP_A));
    limitSlider.max = effectiveLimitMax.toFixed(1);
    if (limit > effectiveLimitMax) {
        limit = effectiveLimitMax;
    }

    setSliderValue(limitSlider, limit);
    setSliderValue(criticalSlider, critical);
    updateSliderValue(limitSlider, limitSpan);
    updateSliderValue(criticalSlider, criticalSpan);
}

function syncAllCurrentLimitPairs(changedSlider = null) {
    syncCurrentLimitPair(dom.vinSlider, dom.vinValueSpan, dom.vinCriticalSlider, dom.vinCriticalValueSpan,
        changedSlider);
    syncCurrentLimitPair(dom.mainSlider, dom.mainValueSpan, dom.mainCriticalSlider, dom.mainCriticalValueSpan,
        changedSlider);
    syncCurrentLimitPair(dom.usbSlider, dom.usbValueSpan, dom.usbCriticalSlider, dom.usbCriticalValueSpan,
        changedSlider);
}

function currentLimitPairIsValid(name, limitSlider, criticalSlider) {
    const limit = parseFloat(limitSlider.value);
    const critical = parseFloat(criticalSlider.value);
    const criticalMin = parseFloat(criticalSlider.min);

    if (critical < criticalMin) {
        alert(`${name} Critical Limit must be at least ${criticalMin.toFixed(1)} A.`);
        return false;
    }

    if (limit > 0 && limit >= critical) {
        alert(`${name} Limit must be lower than Critical Limit.`);
        return false;
    }

    return true;
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
            }
            if (data.vin_critical_current_limit !== undefined) {
                dom.vinCriticalSlider.value = data.vin_critical_current_limit;
            }
            if (data.main_current_limit !== undefined) {
                dom.mainSlider.value = data.main_current_limit;
            }
            if (data.main_critical_current_limit !== undefined) {
                dom.mainCriticalSlider.value = data.main_critical_current_limit;
            }
            if (data.usb_current_limit !== undefined) {
                dom.usbSlider.value = data.usb_current_limit;
            }
            if (data.usb_critical_current_limit !== undefined) {
                dom.usbCriticalSlider.value = data.usb_critical_current_limit;
            }
            syncAllCurrentLimitPairs();
        })
        .catch(error => console.error('Error fetching current limit settings:', error));
}

function setPowerTogglesDisabled(disabled) {
    dom.mainPowerToggle.disabled = disabled;
    dom.usbPowerToggle.disabled = disabled;
}

function postPowerToggleCommand(command) {
    if (powerControlRequestInFlight) {
        return;
    }

    powerControlRequestInFlight = true;
    setPowerTogglesDisabled(true);
    api.postControlCommand(command)
        .catch(error => {
            console.error('Error posting power control command:', error);
            ui.updateControlStatus();
        })
        .finally(() => {
            powerControlRequestInFlight = false;
            setPowerTogglesDisabled(false);
        });
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
    dom.mainPowerToggle.addEventListener('change', () => postPowerToggleCommand({'load_12v_on': dom.mainPowerToggle.checked}));
    dom.usbPowerToggle.addEventListener('change', () => postPowerToggleCommand({'load_5v_on': dom.usbPowerToggle.checked}));
    dom.resetButton.addEventListener('click', () => api.postControlCommand({'reset_trigger': true}));
    dom.powerActionButton.addEventListener('click', () => api.postControlCommand({'power_trigger': true}));

    // --- Settings Modal Controls ---
    dom.scanWifiButton.addEventListener('click', ui.scanForWifi);
    dom.wifiConnectButton.addEventListener('click', ui.connectToWifi);
    dom.networkApplyButton.addEventListener('click', ui.applyNetworkSettings);
    dom.apModeApplyButton.addEventListener('click', ui.applyApModeSettings);
    dom.baudRateApplyButton.addEventListener('click', ui.applyBaudRateSettings);
    dom.periodApplyButton.addEventListener('click', ui.applyPeriodSettings);

    // --- Device Settings (Reboot & Period Slider) ---
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

    if (dom.periodSlider) {
        dom.periodSlider.addEventListener('input', () => {
            dom.periodValue.textContent = dom.periodSlider.value;
        });
    }

    // --- Current Limit Settings ---
    dom.vinSlider.addEventListener('input', () => syncCurrentLimitPair(dom.vinSlider, dom.vinValueSpan,
        dom.vinCriticalSlider, dom.vinCriticalValueSpan, dom.vinSlider));
    dom.vinCriticalSlider.addEventListener('input', () => syncCurrentLimitPair(dom.vinSlider, dom.vinValueSpan,
        dom.vinCriticalSlider, dom.vinCriticalValueSpan, dom.vinCriticalSlider));
    dom.mainSlider.addEventListener('input', () => syncCurrentLimitPair(dom.mainSlider, dom.mainValueSpan,
        dom.mainCriticalSlider, dom.mainCriticalValueSpan, dom.mainSlider));
    dom.mainCriticalSlider.addEventListener('input', () => syncCurrentLimitPair(dom.mainSlider, dom.mainValueSpan,
        dom.mainCriticalSlider, dom.mainCriticalValueSpan, dom.mainCriticalSlider));
    dom.usbSlider.addEventListener('input', () => syncCurrentLimitPair(dom.usbSlider, dom.usbValueSpan,
        dom.usbCriticalSlider, dom.usbCriticalValueSpan, dom.usbSlider));
    dom.usbCriticalSlider.addEventListener('input', () => syncCurrentLimitPair(dom.usbSlider, dom.usbValueSpan,
        dom.usbCriticalSlider, dom.usbCriticalValueSpan, dom.usbCriticalSlider));

    dom.currentLimitApplyButton.addEventListener('click', () => {
        syncAllCurrentLimitPairs();
        if (!currentLimitPairIsValid('VIN', dom.vinSlider, dom.vinCriticalSlider) ||
            !currentLimitPairIsValid('Main', dom.mainSlider, dom.mainCriticalSlider) ||
            !currentLimitPairIsValid('USB', dom.usbSlider, dom.usbCriticalSlider)) {
            return;
        }

        const settings = {
            vin_current_limit: parseFloat(dom.vinSlider.value),
            main_current_limit: parseFloat(dom.mainSlider.value),
            usb_current_limit: parseFloat(dom.usbSlider.value),
            vin_critical_current_limit: parseFloat(dom.vinCriticalSlider.value),
            main_critical_current_limit: parseFloat(dom.mainCriticalSlider.value),
            usb_critical_current_limit: parseFloat(dom.usbCriticalSlider.value)
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
