#!/usr/bin/env node

const baseUrl = process.env.SERVER_API_WS_URL || 'ws://127.0.0.1:7878/ws/v1/events';
const apiKey = process.env.SERVER_API_KEY || '';
const timeoutMs = Number(process.env.SERVER_API_WS_TIMEOUT_MS || 5000);

const options = apiKey ? { headers: { Authorization: `Bearer ${apiKey}` } } : undefined;
const socket = new WebSocket(baseUrl, options);
let subscribed = false;
let pong = false;
let eventReceived = false;
let finished = false;

function fail(message) {
    if (finished) return;
    finished = true;
    console.error(`FAIL: ${message}`);
    try { socket.close(); } catch (_) { }
    process.exit(1);
}

function finish() {
    if (finished || !subscribed || !pong) return;
    finished = true;
    console.log(`PASS: WebSocket subscribe/pong${eventReceived ? ' and event JSON' : ''}`);
    socket.close();
    process.exit(0);
}

const timer = setTimeout(() => {
    if (!subscribed || !pong)
        return fail('subscribe/pong did not complete before timeout');
    if (!eventReceived)
        console.log('INFO: no telemetry event received during timeout; control protocol is valid');
    finish();
}, timeoutMs);

socket.onopen = () => {
    socket.send(JSON.stringify({ type: 'subscribe', events: ['player.position', 'combat.snapshot'] }));
    socket.send(JSON.stringify({ type: 'ping' }));
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
        subscribed = Array.isArray(payload.events) && payload.events.includes('player.position');
    } else if (payload.type === 'pong') {
        pong = true;
    } else if (payload.type === 'player.position' || payload.type === 'combat.snapshot') {
        eventReceived = payload.version === 1 && typeof payload.data === 'object';
        if (!eventReceived)
            fail(`invalid event envelope: ${JSON.stringify(payload)}`);
    }

    if (eventReceived)
        finish();
};

socket.onerror = () => fail('WebSocket connection failed');
socket.onclose = () => {
    clearTimeout(timer);
    if (!finished)
        fail('WebSocket closed before subscribe/pong completed');
};
