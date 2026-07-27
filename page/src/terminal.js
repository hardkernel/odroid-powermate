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
    term = new Terminal({convertEol: true, cursorBlink: true});
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
