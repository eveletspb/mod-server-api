#!/usr/bin/env node

const baseUrl = process.env.SERVER_API_WS_URL || 'ws://127.0.0.1:7878/ws/v1/events';
const timeoutMs = Number(process.env.SERVER_API_WS_TIMEOUT_MS || 5000);

const socket = new WebSocket(baseUrl);
let subscribed = false;
let pong = false;
let publishedEventReceived = false;
let finished = false;

function fail(message) {
    if (finished) return;
    finished = true;
    console.error(`FAIL: ${message}`);
    try { socket.close(); } catch (_) { }
    process.exit(1);
}

function finish() {
    if (finished || !subscribed || !pong || !publishedEventReceived) return;
    finished = true;
    console.log('PASS: WebSocket subscribe/pong/publish and event JSON');
    socket.close();
    process.exit(0);
}

const timer = setTimeout(() => {
    if (!subscribed || !pong)
        return fail('subscribe/pong did not complete before timeout');
    if (!publishedEventReceived)
        return fail('published event was not received before timeout');
    finish();
}, timeoutMs);

socket.onopen = () => {
    socket.send(JSON.stringify({ type: 'subscribe', events: ['provider.smoke'] }));
    socket.send(JSON.stringify({ type: 'ping' }));
    socket.send(JSON.stringify({
        type: 'publish',
        event: 'provider.smoke',
        data: { source: 'test-websocket.js' },
    }));
};

socket.onmessage = (message) => {
    let payload;
    try {
        payload = JSON.parse(message.data.toString());
    } catch (error) {
        fail(`invalid JSON response: ${message.data}`);
        return;
    }

    if (payload.type === 'subscribed') {
        subscribed = Array.isArray(payload.events) && payload.events.includes('provider.smoke');
    } else if (payload.type === 'pong') {
        pong = true;
    } else if (payload.type === 'provider.smoke') {
        publishedEventReceived = payload.version === 1 &&
            payload.data?.source === 'test-websocket.js';
        if (!publishedEventReceived)
            fail(`invalid event envelope: ${JSON.stringify(payload)}`);
    }

    finish();
};

socket.onerror = () => fail('WebSocket connection failed');
socket.onclose = () => {
    clearTimeout(timer);
    if (!finished)
        fail('WebSocket closed before subscribe/pong completed');
};
