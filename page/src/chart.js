/**
 * @file chart.js
 * @description This module manages the Chart.js instances for visualizing sensor data.
 * It handles initialization, theme updates, data updates, and resizing for the three separate charts.
 */

import {Chart, registerables} from 'chart.js';
import {currentChartCtx, graphTabPane, htmlEl, powerChartCtx, voltageChartCtx} from './dom.js';

// Register all necessary Chart.js components
Chart.register(...registerables);

// Store chart instances in an object
export const charts = {
    power: null,
    voltage: null,
    current: null
};

// Configuration for dynamic, step-wise Y-axis scaling
const scaleConfig = {
    power: {steps: [5, 20, 50, 150]},   // in Watts
    voltage: {steps: [5, 10, 15, 25]},    // in Volts
    current: {steps: [1, 2.5, 5, 10]}     // in Amps
};

const channelKeys = ['USB', 'MAIN', 'VIN'];
const CHART_DATA_POINTS = 30; // Number of data points to display on the chart

/**
 * Creates an array of empty labels for initial chart rendering.
 * @returns {Array<string>} An array of empty strings.
 */
function initialLabels() {
    return Array(CHART_DATA_POINTS).fill('');
}

/**
 * Creates an array of null data points for initial chart rendering.
 * @returns {Array<null>} An array of nulls.
 */
function initialData() {
    return Array(CHART_DATA_POINTS).fill(null);
}

/**
 * Creates a common configuration object for a single line chart.
 * @param {string} title - The title of the chart (e.g., 'Power (W)').
 * @returns {Object} A Chart.js options object.
 */
function createChartOptions(title) {
    return {
        responsive: true,
        maintainAspectRatio: false,
        interaction: {mode: 'index', intersect: false},
        plugins: {
            legend: {position: 'top'},
            title: {display: true, text: title}
        },
        scales: {
            x: {ticks: {autoSkipPadding: 10, maxRotation: 0, minRotation: 0}},
            y: {
                min: 0,
                beginAtZero: true,
                ticks: {}
            }
        }
    };
}

/**
 * Creates the dataset objects for a chart.
 * @param {string} unit - The unit for the dataset label (e.g., 'W', 'V', 'A').
 * @returns {Array<Object>} An array of Chart.js dataset objects.
 */
function createDatasets(unit) {
    return channelKeys.map(channel => ({
        label: `${channel} (${unit})`,
        data: initialData(),
        borderWidth: 2,
        fill: false,
        tension: 0.2,
        pointRadius: 0
    }));
}

/**
 * Initializes a single chart with its specific configuration.
 * @param {CanvasRenderingContext2D} context - The canvas context for the chart.
 * @param {string} title - The chart title.
 * @param {string} metric - The metric key ('power', 'voltage', 'current').
 * @param {string} unit - The data unit ('W', 'V', 'A').
 * @returns {Chart} A new Chart.js instance.
 */
function initializeSingleChart(context, title, metric, unit) {
    if (!context) return null;

    const options = createChartOptions(title);
    const initialMax = scaleConfig[metric].steps[0];
    options.scales.y.max = initialMax;
    options.scales.y.ticks.stepSize = initialMax / 5; // Initial step size

    return new Chart(context, {
        type: 'line',
        data: {labels: initialLabels(), datasets: createDatasets(unit)},
        options: options
    });
}

/**
 * Initializes all three charts (Power, Voltage, Current).
 * If chart instances already exist, they are destroyed and new ones are created.
 */
export function initCharts() {
    // Destroy existing charts if they exist
    Object.keys(charts).forEach(key => {
        if (charts[key]) {
            charts[key].destroy();
        }
    });

    charts.power = initializeSingleChart(powerChartCtx, 'Power', 'power', 'W');
    charts.voltage = initializeSingleChart(voltageChartCtx, 'Voltage', 'voltage', 'V');
    charts.current = initializeSingleChart(currentChartCtx, 'Current', 'current', 'A');
}

/**
 * Applies a new theme (light or dark) to all charts.
 * @param {string} themeName - The name of the theme to apply ('light' or 'dark').
 */
export function applyChartsTheme(themeName) {
    const isDark = themeName === 'dark';
    const gridColor = isDark ? 'rgba(255, 255, 255, 0.1)' : 'rgba(0, 0, 0, 0.1)';
    const labelColor = isDark ? '#dee2e6' : '#212529';

    const channelColors = [
        getComputedStyle(htmlEl).getPropertyValue('--chart-usb-color').trim() || '#0d6efd',
        getComputedStyle(htmlEl).getPropertyValue('--chart-main-color').trim() || '#198754',
        getComputedStyle(htmlEl).getPropertyValue('--chart-vin-color').trim() || '#dc3545'
    ];

    const updateThemeForChart = (chart) => {
        if (!chart) return;
        chart.options.scales.x.grid.color = gridColor;
        chart.options.scales.y.grid.color = gridColor;
        chart.options.scales.x.ticks.color = labelColor;
        chart.options.scales.y.ticks.color = labelColor;
        chart.options.plugins.legend.labels.color = labelColor;
        chart.options.plugins.title.color = labelColor;

        chart.data.datasets.forEach((dataset, index) => {
            dataset.borderColor = channelColors[index];
        });

        chart.update('none');
    };

    Object.values(charts).forEach(updateThemeForChart);
}

/**
 * Updates a single chart with new data and dynamically adjusts its Y-axis scale.
 * @param {Chart} chart - The Chart.js instance to update.
 * @param {string} metric - The metric key (e.g., 'power', 'voltage').
 * @param {Object} data - The new sensor data object.
 * @param {string} timeLabel - The timestamp label for the new data point.
 */
function updateSingleChart(chart, metric, data, timeLabel) {
    if (!chart) return;

    // Shift old data and push new data
    chart.data.labels.shift();
    chart.data.labels.push(timeLabel);
    chart.data.datasets.forEach((dataset, index) => {
        dataset.data.shift();
        const channel = channelKeys[index];
        const value = data[channel]?.[metric];
        dataset.data.push(value !== undefined ? value.toFixed(2) : null);
    });

    // --- DYNAMIC STEP-WISE Y-AXIS SCALING ---
    const config = scaleConfig[metric];
    if (config?.steps) {
        const allData = chart.data.datasets
            .flatMap(dataset => dataset.data)
            .filter(v => v !== null)
            .map(v => parseFloat(v));

        const maxDataValue = allData.length > 0 ? Math.max(...allData) : 0;

        // Find the smallest step that is >= maxDataValue
        let newMax = config.steps.find(step => maxDataValue <= step);

        // If value exceeds all steps, use the largest step. If no data, use the smallest.
        if (newMax === undefined) {
            newMax = config.steps[config.steps.length - 1];
        }

        if (chart.options.scales.y.max !== newMax) {
            chart.options.scales.y.max = newMax;
            // Dynamically adjust stepSize for clearer grid lines
            chart.options.scales.y.ticks.stepSize = newMax / 5;
        }
    }
    // --- END DYNAMIC SCALING ---

    // Update chart only if its tab is visible for performance.
    if (graphTabPane.classList.contains('show')) {
        chart.update('none');
    }
}


/**
 * Updates all charts with new sensor data.
 * @param {Object} data - The new sensor data object from the WebSocket.
 */
export function updateCharts(data) {
    const timeLabel = new Date(data.timestamp * 1000).toLocaleTimeString();

    updateSingleChart(charts.power, 'power', data, timeLabel);
    updateSingleChart(charts.voltage, 'voltage', data, timeLabel);
    updateSingleChart(charts.current, 'current', data, timeLabel);
}

/**
 * Resizes all chart canvases. This is typically called on window resize events.
 */
export function resizeCharts() {
    Object.values(charts).forEach(chart => {
        if (chart) {
            chart.resize();
        }
    });
}
