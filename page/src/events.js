// SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
// SPDX-License-Identifier: GPL-3.0-only

/**
 * @file events.js
 * @description This module sets up all the event listeners for the application.
 * It connects user interactions (like clicks and toggles) to the corresponding
 * functions in other modules (UI, API, etc.).
 */

import {initTlsSettings} from './tls.js';
import * as dom from './dom.js';
import * as api from './api.js';
import {getAuthHeaders, handleResponse} from './api.js'; // Import auth functions
import * as ui from './ui.js';
import {clearTerminal, downloadTerminalOutput, fitTerminal} from './terminal.js';
import {debounce, isMobile} from './utils.js';
import {getLastStatusMessageReceivedAtMs} from './websocket.js';

// A flag to track if charts have been initialized
let chartsInitialized = false;
let listenersAttached = false;
let powerControlRequestInFlight = false;
let diagnosticsRefreshTimer = null;
let diagnosticsRequestInFlight = false;
const DIAGNOSTICS_REFRESH_INTERVAL_MS = 2000;
const CURRENT_LIMIT_STEP_A = 0.1;
const RECOMMENDED_CURRENT_LIMITS_A = {
    VIN: 9.0,
    MAIN: 7.0,
    USB: 4.0,
};
// The firmware holds the power line 3 s and the reset line 1 s by default, and refuses a
// press while the previous one is held (HTTP 409).
const BUTTON_HOLD_MS = {power_trigger: 3500, reset_trigger: 1500};
// The limits as read from the device; Apply sends only the pairs that differ from it.
let loadedCurrentLimits = null;
// Only the latest load may update the sliders and Apply: an older one can finish later.
let currentLimitLoadSeq = 0;

function formatBytes(bytes) {
    if (!Number.isFinite(bytes)) return '-';
    if (bytes < 1024) return `${bytes} B`;
    return `${(bytes / 1024).toFixed(1)} KiB`;
}

function formatUptime(seconds) {
    if (!Number.isFinite(seconds)) return '-';
    const days = Math.floor(seconds / 86400);
    const hours = Math.floor((seconds % 86400) / 3600);
    const minutes = Math.floor((seconds % 3600) / 60);
    const remainingSeconds = seconds % 60;
    return days > 0 ? `${days}d ${hours}h ${minutes}m` : `${hours}h ${minutes}m ${remainingSeconds}s`;
}

function formatElapsedSeconds(seconds) {
    if (!Number.isFinite(seconds) || seconds < 0) return '-';

    const wholeSeconds = Math.floor(seconds);
    const days = Math.floor(wholeSeconds / 86400);
    const hours = Math.floor((wholeSeconds % 86400) / 3600);
    const minutes = Math.floor((wholeSeconds % 3600) / 60);
    const remainingSeconds = wholeSeconds % 60;

    if (days > 0) return `${days}d ${hours}h`;
    if (hours > 0) return `${hours}h ${minutes}m`;
    if (minutes > 0) return `${minutes}m ${remainingSeconds}s`;
    return `${remainingSeconds}s`;
}

function formatState(state) {
    if (!state) return 'Unknown';
    return state.charAt(0).toUpperCase() + state.slice(1);
}

async function refreshDiagnostics() {
    if (!dom.diagnosticsRefreshButton || diagnosticsRequestInFlight) return;

    diagnosticsRequestInFlight = true;

    dom.diagnosticsRefreshButton.disabled = true;
    dom.diagnosticsStatus.textContent = 'Refreshing...';
    dom.diagnosticsStatus.classList.remove('text-danger');
    dom.diagnosticsStatus.classList.add('text-secondary');

    try {
        const data = await api.fetchDiagnostics();
        dom.diagnosticsUptime.textContent = formatUptime(data.uptime_seconds);
        dom.diagnosticsHeap.textContent = `${formatBytes(data.free_heap_bytes)} free / ${formatBytes(data.minimum_free_heap_bytes)} min`;
        dom.diagnosticsLargestBlock.textContent = formatBytes(data.largest_free_block_bytes);
        dom.diagnosticsHttpClients.textContent = `${data.http_clients} active`;
        dom.diagnosticsWebsocket.textContent = `${data.websocket_clients} clients, ${data.websocket_queue_depth}/${data.websocket_queue_capacity} queued`;
        dom.diagnosticsUart.textContent = `${formatBytes(data.uart_buffered_bytes)} buffered, ${formatBytes(data.uart_received_bytes)} received`;
        dom.diagnosticsUartErrors.textContent = `FIFO ${data.uart_fifo_overflows}, buffer ${data.uart_buffer_full_events}`;
        dom.diagnosticsQueueDrops.textContent = `UART ${data.uart_queue_drops}, status ${data.status_queue_drops}`;
        dom.diagnosticsWsFailures.textContent = data.websocket_send_failures;
        dom.diagnosticsWifi.textContent = data.wifi_connected ? `Connected (${data.wifi_rssi} dBm)` : 'Disconnected';

        const staState = data.wifi_sta_state || 'unknown';
        dom.diagnosticsWifiStaState.textContent = formatState(staState);

        const disconnectReason = data.wifi_last_disconnect_reason;
        dom.diagnosticsWifiLastDisconnect.textContent = disconnectReason
            ? `${disconnectReason} (${data.wifi_last_disconnect_reason_code})`
            : 'None';

        const reconnectBackoffMs = Number(data.wifi_reconnect_backoff_ms);
        if (staState === 'connecting') {
            dom.diagnosticsWifiBackoff.textContent = reconnectBackoffMs > 0
                ? `${(reconnectBackoffMs / 1000).toFixed(1)} s`
                : 'Connecting now';
        } else {
            dom.diagnosticsWifiBackoff.textContent = 'Inactive';
        }

        if (data.wifi_has_connected && Number.isFinite(data.wifi_last_connected_uptime_seconds)) {
            const connectedAgeSeconds = Math.max(
                0, data.uptime_seconds - data.wifi_last_connected_uptime_seconds);
            dom.diagnosticsWifiLastConnected.textContent =
                `${formatElapsedSeconds(connectedAgeSeconds)} ago`;
        } else {
            dom.diagnosticsWifiLastConnected.textContent = 'Never';
        }

        const netType = data.wifi_net_type === 'static'
            ? 'Static'
            : data.wifi_net_type === 'dhcp' ? 'DHCP' : 'Unknown';
        dom.diagnosticsWifiNetwork.textContent =
            `${netType} · ${data.wifi_ip_address || 'No IP'}` +
            (data.wifi_static_ip_error ? ` (saved static IP not used: ${data.wifi_static_ip_error})` : '');
        dom.diagnosticsWifiRoute.textContent =
            data.wifi_gateway && data.wifi_netmask
                ? `${data.wifi_gateway} / ${data.wifi_netmask}`
                : '-';

        const lastStatusMessageAtMs = getLastStatusMessageReceivedAtMs();
        dom.diagnosticsLastStatusMessage.textContent = lastStatusMessageAtMs > 0
            ? `${formatElapsedSeconds(Math.max(0, Date.now() - lastStatusMessageAtMs) / 1000)} ago`
            : 'Not received';

        dom.diagnosticsStatus.textContent =
            `Updated ${new Date().toLocaleTimeString()} (auto-refresh: ${DIAGNOSTICS_REFRESH_INTERVAL_MS / 1000} s)`;
    } catch (error) {
        console.error('Error fetching diagnostics:', error);
        dom.diagnosticsStatus.textContent = 'Failed to fetch diagnostics';
        dom.diagnosticsStatus.classList.remove('text-secondary');
        dom.diagnosticsStatus.classList.add('text-danger');
    } finally {
        diagnosticsRequestInFlight = false;
        dom.diagnosticsRefreshButton.disabled = false;
    }
}

function diagnosticsTabIsActive() {
    return document.getElementById('debug-tab-pane')?.classList.contains('active');
}

function startDiagnosticsRefresh() {
    if (diagnosticsRefreshTimer || document.visibilityState !== 'visible') return;

    refreshDiagnostics();
    diagnosticsRefreshTimer = setInterval(refreshDiagnostics, DIAGNOSTICS_REFRESH_INTERVAL_MS);
}

function stopDiagnosticsRefresh() {
    if (diagnosticsRefreshTimer) {
        clearInterval(diagnosticsRefreshTimer);
        diagnosticsRefreshTimer = null;
    }
}

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

function updateRecommendedCurrentLimitWarning() {
    if (!dom.recommendedCurrentLimitWarning || !dom.recommendedCurrentLimitWarningMessage) return;

    const limits = [
        ['VIN', dom.vinSlider, RECOMMENDED_CURRENT_LIMITS_A.VIN],
        ['MAIN', dom.mainSlider, RECOMMENDED_CURRENT_LIMITS_A.MAIN],
        ['USB', dom.usbSlider, RECOMMENDED_CURRENT_LIMITS_A.USB],
    ];
    const disabled = limits
        .filter(([, slider]) => parseFloat(slider.value) === 0)
        .map(([name]) => name);
    const exceeded = limits
        .filter(([, slider, recommended]) => parseFloat(slider.value) >= recommended)
        .map(([name, , recommended]) => `${name} \u2265 ${recommended.toFixed(1)} A`);
    const messages = [];

    if (disabled.length > 0) {
        messages.push(`Current-limit protection disabled: ${disabled.join(', ')}.`);
    }
    if (exceeded.length > 0) {
        messages.push(`At or above recommended continuous limits: ${exceeded.join(', ')}. ` +
            'Brief current peaks are tolerated, but sustained operation is not recommended.');
    }

    dom.recommendedCurrentLimitWarning.classList.toggle('d-none', messages.length === 0);
    dom.recommendedCurrentLimitWarningMessage.textContent =
        messages.join(' ');
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
    updateRecommendedCurrentLimitWarning();
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

const CURRENT_LIMIT_PAIRS = [
    ['VIN', 'vin', () => dom.vinSlider, () => dom.vinCriticalSlider],
    ['Main', 'main', () => dom.mainSlider, () => dom.mainCriticalSlider],
    ['USB', 'usb', () => dom.usbSlider, () => dom.usbCriticalSlider],
];

function readCurrentLimits() {
    const limits = {};
    for (const [, key, limitSlider, criticalSlider] of CURRENT_LIMIT_PAIRS) {
        limits[`${key}_current_limit`] = parseFloat(limitSlider().value);
        limits[`${key}_critical_current_limit`] = parseFloat(criticalSlider().value);
    }
    return limits;
}

function setCurrentLimitsLoaded(loaded) {
    if (!loaded) loadedCurrentLimits = null;
    dom.currentLimitApplyButton.disabled = !loaded;
    if (dom.currentLimitLoadError) dom.currentLimitLoadError.hidden = loaded;
}

// Locked from Apply until the reload after it settles, which rewrites the sliders: an
// edit made in between would be lost.
function setCurrentLimitSlidersDisabled(disabled) {
    for (const [, , limitSlider, criticalSlider] of CURRENT_LIMIT_PAIRS) {
        limitSlider().disabled = disabled;
        criticalSlider().disabled = disabled;
    }
}

function loadCurrentLimitSettings() {
    // Until the device's values are in, the sliders hold their HTML defaults: Apply would
    // write those over the real limits.
    setCurrentLimitsLoaded(false);
    if (dom.currentLimitLoadError) dom.currentLimitLoadError.hidden = true;
    const seq = ++currentLimitLoadSeq;
    fetch('/api/setting', {
        headers: getAuthHeaders(), // Add auth headers
    })
        .then(handleResponse) // Handle response for 401
        .then(response => response.json())
        .then(data => {
            if (seq !== currentLimitLoadSeq) return;
            const keys = CURRENT_LIMIT_PAIRS.flatMap(([, key]) => [`${key}_current_limit`, `${key}_critical_current_limit`]);
            setCurrentLimitSlidersDisabled(false);
            if (keys.some(key => typeof data[key] !== 'number')) {
                throw new Error('current limits missing from /api/setting');
            }
            // A max that the last sync lowered would clamp the value assigned below.
            for (const [, , limitSlider] of CURRENT_LIMIT_PAIRS) {
                limitSlider().max = limitSlider().dataset.max;
            }
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
            loadedCurrentLimits = readCurrentLimits();
            setCurrentLimitsLoaded(true);
        })
        .catch(error => {
            console.error('Error fetching current limit settings:', error);
            if (seq === currentLimitLoadSeq) {
                setCurrentLimitsLoaded(false);
                setCurrentLimitSlidersDisabled(false);
            }
        });
}

/** Explains a reply whose status is 'error', from its per-limit results. */
function currentLimitErrors(data) {
    const results = data.climit_results ?? {};
    const lines = Object.entries(results)
        .filter(([, result]) => result?.status === 'error')
        .map(([key, result]) => `${key}: ${result.error}`);
    return lines.length ? lines.join('\n') : (data.message || 'the device reported an error');
}

function pressButton(button, command) {
    if (button.disabled) return;
    button.disabled = true;
    const hold = BUTTON_HOLD_MS[Object.keys(command)[0]] ?? 1500;
    api.postControlCommand(command)
        .then(() => new Promise(resolve => setTimeout(resolve, hold)))
        .catch(error => {
            console.error('Error posting button command:', error);
            alert(`Button press failed: ${error.message}`);
        })
        .finally(() => {
            button.disabled = false;
        });
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
    // Disabled while the line is held: a second press would be refused (409).
    dom.resetButton.addEventListener('click', () => pressButton(dom.resetButton, {'reset_trigger': true}));
    dom.powerActionButton.addEventListener('click', () => pressButton(dom.powerActionButton, {'power_trigger': true}));
    if (dom.diagnosticsRefreshButton) {
        dom.diagnosticsRefreshButton.addEventListener('click', refreshDiagnostics);
    }

    // --- Settings Modal Controls ---
    dom.scanWifiButton.addEventListener('click', ui.scanForWifi);
    dom.wifiConnectButton.addEventListener('click', ui.connectToWifi);
    dom.networkApplyButton.addEventListener('click', ui.applyNetworkSettings);
    dom.apModeApplyButton.addEventListener('click', ui.applyApModeSettings);
    dom.baudRateApplyButton.addEventListener('click', ui.applyBaudRateSettings);
    dom.periodApplyButton.addEventListener('click', ui.applyPeriodSettings);
    dom.restoreOutputStateApplyButton.addEventListener('click', ui.applyRestoreOutputStateSetting);

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
        if (!loadedCurrentLimits) {
            alert('The current limits were not read from the device. Close and reopen Settings to try again.');
            return;
        }
        syncAllCurrentLimitPairs();
        if (!currentLimitPairIsValid('VIN', dom.vinSlider, dom.vinCriticalSlider) ||
            !currentLimitPairIsValid('Main', dom.mainSlider, dom.mainCriticalSlider) ||
            !currentLimitPairIsValid('USB', dom.usbSlider, dom.usbCriticalSlider)) {
            return;
        }

        // Only the pairs the user changed: an untouched pair is not rewritten with the
        // slider's 0.1 A rounding of a value set elsewhere.
        const current = readCurrentLimits();
        const settings = {};
        for (const [, key] of CURRENT_LIMIT_PAIRS) {
            const limitKey = `${key}_current_limit`;
            const criticalKey = `${key}_critical_current_limit`;
            if (current[limitKey] !== loadedCurrentLimits[limitKey] ||
                current[criticalKey] !== loadedCurrentLimits[criticalKey]) {
                settings[limitKey] = current[limitKey];
                settings[criticalKey] = current[criticalKey];
            }
        }
        if (Object.keys(settings).length === 0) {
            alert('No current limit was changed.');
            return;
        }
        dom.currentLimitApplyButton.disabled = true;
        setCurrentLimitSlidersDisabled(true);

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
                if (data.status === 'error') {
                    alert(`Some current limits were not applied:\n${currentLimitErrors(data)}`);
                }
            })
            .catch((error) => {
                console.error('Error applying current limit settings:', error);
                alert('Failed to apply current limit settings.');
            })
            .finally(loadCurrentLimitSettings); // show what the device now holds
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
        initTlsSettings();
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

            if (tabId === '#debug-tab-pane') {
                startDiagnosticsRefresh();
            } else {
                stopDiagnosticsRefresh();
            }

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

    document.addEventListener('visibilitychange', () => {
        if (document.visibilityState === 'visible' && diagnosticsTabIsActive()) {
            startDiagnosticsRefresh();
        } else {
            stopDiagnosticsRefresh();
        }
    });

    // --- Window Resize Event ---
    // Debounced to avoid excessive calls during resizing.
    window.addEventListener('resize', debounce(ui.handleResize, 150));

    listenersAttached = true;
}
