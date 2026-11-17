// SPDX-FileCopyrightText: 2025 Hardkernel Co., Ltd.
// SPDX-License-Identifier: GPL-3.0-only

/**
 * @file terminal.js
 * @description This module manages the Xterm.js terminal instance, including setup,
 * theme handling, and data communication with the WebSocket.
 */

import {Terminal} from '@xterm/xterm';
import '@xterm/xterm/css/xterm.css';
import {FitAddon} from '@xterm/addon-fit';
import {terminalContainer} from './dom.js';
import {isMobile} from './utils.js';
import {sendUartMessage} from './uart-websocket.js';

// Exported terminal instance and addon for global access
export let term;
export let fitAddon;

// Theme definitions for the terminal
const lightTheme = {
    background: 'transparent',
    foreground: '#212529',
    cursor: '#212529',
    selectionBackground: 'rgba(0,0,0,0.1)'
};

const darkTheme = {
    background: 'transparent',
    foreground: '#dee2e6',
    cursor: '#dee2e6',
    selectionBackground: 'rgba(255,255,255,0.1)'
};

/**
 * Initializes the Xterm.js terminal, loads addons, and attaches it to the DOM.
 * Sets up the initial size and the data handler for sending input to the WebSocket.
 */
export function setupTerminal() {
    // Ensure the container is empty before creating a new terminal
    while (terminalContainer.firstChild) {
        terminalContainer.removeChild(terminalContainer.firstChild);
    }

    fitAddon = new FitAddon();
    // The page theme was applied before this terminal existed, so start in it; xterm's own
    // default is white text, invisible on the light theme's transparent background.
    const isDark = document.documentElement.getAttribute('data-bs-theme') === 'dark';
    term = new Terminal({convertEol: true, cursorBlink: true, theme: isDark ? darkTheme : lightTheme});
    term.loadAddon(fitAddon);
    term.open(terminalContainer);

    // Adjust terminal size based on device type
    if (isMobile()) {
        fitAddon.fit();
    } else {
        term.resize(80, 24); // Default size for desktop
    }

    // Handle user input and send it over the WebSocket
    term.onData(data => {
        sendUartMessage(data);
    });

    // Ctrl+C (or Ctrl+Shift+C, Cmd+C) copies the selection; with nothing selected Ctrl+C
    // still goes to the SBC as an interrupt.
    term.attachCustomKeyEventHandler(event => {
        const copyKey = (event.ctrlKey || event.metaKey) && !event.altKey && event.key.toLowerCase() === 'c';
        if (event.type !== 'keydown' || !copyKey || !term.hasSelection()) return true;
        event.preventDefault();
        copyToClipboard(term.getSelection());
        term.clearSelection();
        return false;
    });
}

/** execCommand('copy') through a hidden textarea; the only copy path left on plain HTTP. */
function legacyCopy(text) {
    const area = document.createElement('textarea');
    area.value = text;
    area.setAttribute('readonly', '');
    area.style.position = 'fixed';
    area.style.opacity = '0';
    document.body.appendChild(area);
    area.select();
    let copied = false;
    try {
        copied = document.execCommand('copy');
    } catch {
        copied = false;
    }
    area.remove();
    term.focus();
    return copied;
}

/**
 * Copies text to the clipboard. The Clipboard API needs HTTPS; on plain HTTP the older
 * copy command is tried, and when the browser refuses that too the user is told why.
 */
async function copyToClipboard(text) {
    if (window.isSecureContext && navigator.clipboard) {
        try {
            await navigator.clipboard.writeText(text);
            return;
        } catch (error) {
            console.warn('Clipboard write failed:', error);
        }
    }
    if (legacyCopy(text)) return;
    alert(window.isSecureContext
        ? 'The browser did not allow copying to the clipboard.'
        : 'Copying is not available over plain HTTP: the browser blocks clipboard access. ' +
          'Turn on HTTPS in Settings > TLS, or use the download button to save the log.');
}

/**
 * Applies a new theme (light or dark) to the terminal.
 * @param {string} themeName - The name of the theme to apply ('light' or 'dark').
 */
export function applyTerminalTheme(themeName) {
    if (!term) return;
    const isDark = themeName === 'dark';
    term.options.theme = isDark ? darkTheme : lightTheme;
}

/**
 * Clears the terminal screen.
 */
export function clearTerminal() {
    if (term) {
        term.clear();
    }
}

/**
 * Fits the terminal to the size of its container element.
 * Useful for responsive adjustments on window resize.
 */
export function fitTerminal() {
    if (fitAddon) {
        fitAddon.fit();
    }
}

/**
 * Gathers all text from the terminal buffer and downloads it as a .log file.
 */
export function downloadTerminalOutput() {
    if (!term) {
        console.error("Terminal is not initialized.");
        return;
    }

    const buffer = term.buffer.active;
    let fullText = '';

    // Iterate through the buffer to get all lines
    for (let i = 0; i < buffer.length; i++) {
        const line = buffer.getLine(i);
        if (line) {
            fullText += line.translateToString() + '\n';
        }
    }

    if (fullText.trim() === '') {
        console.warn("Terminal is empty, nothing to download.");
        // Optionally, provide user feedback here (e.g., a toast notification)
        return;
    }

    // Create a blob from the text content
    const blob = new Blob([fullText], {type: 'text/plain;charset=utf-8'});

    // Create a link element to trigger the download
    const link = document.createElement('a');
    const url = URL.createObjectURL(blob);

    // Generate a filename with a timestamp
    const timestamp = new Date().toISOString().replace(/[:.]/g, '-');
    link.download = `odroid-log-${timestamp}.log`;
    link.href = url;

    // Append to the document, click, and then remove
    document.body.appendChild(link);
    link.click();
    document.body.removeChild(link);

    // Clean up the object URL
    URL.revokeObjectURL(url);
}
