/**
 * @file utils.js
 * @description Provides utility functions used throughout the application.
 */

/**
 * Creates a debounced function that delays invoking `func` until after `delay` milliseconds
 * have elapsed since the last time the debounced function was invoked.
 * @param {Function} func The function to debounce.
 * @param {number} delay The number of milliseconds to delay.
 * @returns {Function} Returns the new debounced function.
 */
export function debounce(func, delay) {
    let timeout;
    return function (...args) {
        const context = this;
        clearTimeout(timeout);
        timeout = setTimeout(() => func.apply(context, args), delay);
    };
}

/**
 * Formats a duration in total seconds into a human-readable string (e.g., "2days 02:30:15").
 * @param {number} totalSeconds The total seconds to format.
 * @returns {string} The formatted uptime string.
 */
export function formatUptime(totalSeconds) {
    totalSeconds = Math.floor(totalSeconds);
    const days = Math.floor(totalSeconds / 86400);
    const hours = Math.floor((totalSeconds % 86400) / 3600);
    const minutes = Math.floor((totalSeconds % 3600) / 60);
    const seconds = totalSeconds % 60;
    const pad = (num) => String(num).padStart(2, '0');
    const timeString = `${pad(hours)}:${pad(minutes)}:${pad(seconds)}`;
    return days > 0 ? `${days}days ${timeString}` : timeString;
}

/**
 * Checks if the current device is likely a mobile device based on screen width.
 * @returns {boolean} True if the window width is less than or equal to 767.98px, false otherwise.
 */
export function isMobile() {
    // The 767.98px breakpoint is typically used by Bootstrap for medium (md) devices.
    return window.innerWidth <= 767.98;
}
