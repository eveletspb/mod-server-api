#!/usr/bin/env node

const wsUrl = process.env.SERVER_API_WS_URL || 'ws://127.0.0.1:7878/ws/v1/events';
const rate = Number(process.env.SERVER_API_WS_PUBLISH_RATE || 10000);
const durationMs = Number(process.env.SERVER_API_WS_DURATION_MS || 5000);
const metricsUrl = process.env.SERVER_API_METRICS_URL || (() => {
    const url = new URL(wsUrl);
    url.protocol = url.protocol === 'wss:' ? 'https:' : 'http:';
    url.pathname = '/api/v1/server/metrics';
    url.search = '';
    return url.toString();
})();

if (!Number.isInteger(rate) || rate < 1)
    throw new Error('SERVER_API_WS_PUBLISH_RATE must be a positive integer');

const socket = new WebSocket(wsUrl);
const latencies = [];
let startedAt = 0;
let sent = 0;
let received = 0;
let invalid = 0;
let done = false;
let finishTimer;

function percentile(values, fraction) {
    if (values.length === 0) return null;
    return values[Math.min(values.length - 1, Math.floor(values.length * fraction))];
}

async function finish(reason) {
    if (done) return;
    done = true;
    clearTimeout(finishTimer);
    try { socket.close(); } catch (_) { }

    let metrics = null;
    try {
        const headers = process.env.SERVER_API_AUTH_TOKEN
            ? { Authorization: `Bearer ${process.env.SERVER_API_AUTH_TOKEN}` }
            : {};
        const response = await fetch(metricsUrl, { headers });
        if (response.ok) metrics = await response.json();
    } catch (_) { }

    const elapsedMs = startedAt ? Date.now() - startedAt : 0;
    console.log(JSON.stringify({
        reason,
        targetMessagesPerSecond: rate,
        sent,
        received,
        invalid,
        elapsedMs,
        measuredSendRate: elapsedMs ? Math.round(sent * 1000 / elapsedMs) : 0,
        latencyMs: latencies.length ? {
            mean: Number((latencies.reduce((sum, value) => sum + value, 0) / latencies.length).toFixed(2)),
            p95: percentile(latencies, 0.95),
            max: Math.max(...latencies),
        } : null,
        metrics,
    }));
    if (reason !== 'completed') process.exitCode = 1;
}

socket.onopen = () => {
    socket.send(JSON.stringify({ type: 'subscribe', events: ['provider.load'] }));
};

socket.onmessage = (message) => {
    let payload;
    try {
        payload = JSON.parse(message.data.toString());
    } catch (_) {
        ++invalid;
        return;
    }

    if (payload.type === 'subscribed') {
        startedAt = Date.now();
        const intervalMs = 10;
        const timer = setInterval(() => {
            const elapsedMs = Date.now() - startedAt;
            const due = Math.min(rate * durationMs / 1000, Math.floor(rate * elapsedMs / 1000));
            const batchEnd = Math.min(due, sent + Math.ceil(rate * intervalMs / 1000));
            while (sent < batchEnd && socket.readyState === WebSocket.OPEN) {
                const sequence = sent++;
                socket.send(JSON.stringify({
                    type: 'publish',
                    event: 'provider.load',
                    data: { sequence: String(sequence), sentAtMs: String(Date.now()) },
                }));
            }
            if (elapsedMs >= durationMs) {
                clearInterval(timer);
                finishTimer = setTimeout(() => finish('completed'), Math.max(5000, durationMs));
            }
        }, intervalMs);
    } else if (payload.type === 'provider.load') {
        ++received;
        const sentAtMs = Number(payload.data?.sentAtMs);
        if (Number.isFinite(sentAtMs)) latencies.push(Math.max(0, Date.now() - sentAtMs));
        else ++invalid;
    }
};

socket.onerror = () => finish('connection-error');
socket.onclose = () => {
    if (!done) finish('connection-closed');
};

setTimeout(() => {
    if (!done) finish('timeout');
}, durationMs * 3 + 10000);
