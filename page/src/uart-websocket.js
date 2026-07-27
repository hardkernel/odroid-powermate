/** Manages the raw UART stream independently from protobuf status messages. */

let uartWebSocket;

const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
const baseGateway = `${protocol}//${window.location.host}/uart`;

export function connectUartStream(onMessage) {
    if (uartWebSocket && (uartWebSocket.readyState === WebSocket.OPEN || uartWebSocket.readyState === WebSocket.CONNECTING)) return;

    const token = localStorage.getItem('authToken');
    if (!token) return;

    uartWebSocket = new WebSocket(`${baseGateway}?token=${token}`);
    uartWebSocket.binaryType = 'arraybuffer';
    uartWebSocket.onmessage = onMessage;
    uartWebSocket.onclose = () => {
        uartWebSocket = undefined;
    };
}

export function disconnectUartStream() {
    if (uartWebSocket) {
        uartWebSocket.close();
        uartWebSocket = undefined;
    }
}

export function sendUartMessage(data) {
    if (uartWebSocket && uartWebSocket.readyState === WebSocket.OPEN) uartWebSocket.send(data);
}
