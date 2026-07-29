/** Manages the raw UART stream independently from protobuf status messages. */

let uartWebSocket;
let cancelUartConnection;

const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
const baseGateway = `${protocol}//${window.location.host}/uart`;
const UART_CONNECTION_TIMEOUT = 10000;

export function connectUartStream({onOpen, onMessage, onClose}) {
    if (uartWebSocket && (uartWebSocket.readyState === WebSocket.OPEN || uartWebSocket.readyState === WebSocket.CONNECTING)) return;

    const token = localStorage.getItem('authToken');
    if (!token) return;

    const socket = new WebSocket(`${baseGateway}?token=${token}`);
    let closeNotified = false;
    let connectionTimeoutId;
    uartWebSocket = socket;
    socket.binaryType = 'arraybuffer';

    const finishConnection = (event, notify) => {
        if (closeNotified) return;
        closeNotified = true;

        if (connectionTimeoutId) {
            clearTimeout(connectionTimeoutId);
            connectionTimeoutId = null;
        }
        if (uartWebSocket === socket) {
            uartWebSocket = undefined;
            cancelUartConnection = undefined;
        }

        socket.onopen = null;
        socket.onmessage = null;
        socket.onclose = null;
        socket.onerror = null;

        if (notify && onClose) onClose(event);
    };

    const closeConnection = (event, notify) => {
        finishConnection(event, notify);
        if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING) {
            socket.close();
        }
    };

    cancelUartConnection = () => closeConnection(undefined, false);

    socket.onopen = (event) => {
        if (uartWebSocket !== socket || closeNotified) return;
        if (connectionTimeoutId) {
            clearTimeout(connectionTimeoutId);
            connectionTimeoutId = null;
        }
        if (onOpen) onOpen(event);
    };
    socket.onmessage = (event) => {
        if (uartWebSocket === socket && !closeNotified && onMessage) onMessage(event);
    };
    socket.onclose = (event) => {
        finishConnection(event, true);
    };
    socket.onerror = (error) => {
        if (uartWebSocket === socket && !closeNotified) {
            console.warn('UART WebSocket error:', error);
            closeConnection({
                type: 'error',
                wasClean: false,
                code: 1006,
                reason: 'WebSocket error',
            }, true);
        }
    };

    connectionTimeoutId = setTimeout(() => {
        if (uartWebSocket !== socket || closeNotified) return;
        console.warn('UART WebSocket: Connection attempt timed out.');
        closeConnection({
            type: 'connection-timeout',
            wasClean: false,
            code: 1006,
            reason: 'Connection timeout',
        }, true);
    }, UART_CONNECTION_TIMEOUT);
}

export function disconnectUartStream() {
    if (cancelUartConnection) {
        cancelUartConnection();
        return;
    }

    const socket = uartWebSocket;
    uartWebSocket = undefined;
    if (!socket) return;

    socket.onopen = null;
    socket.onmessage = null;
    socket.onclose = null;
    socket.onerror = null;
    if (socket.readyState === WebSocket.OPEN || socket.readyState === WebSocket.CONNECTING) {
        socket.close();
    }
}

export function sendUartMessage(data) {
    if (uartWebSocket && uartWebSocket.readyState === WebSocket.OPEN) uartWebSocket.send(data);
}
