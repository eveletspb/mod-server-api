#!/usr/bin/env node

const url = process.env.SERVER_API_WS_URL || 'ws://127.0.0.1:7878/ws/v1/events';
const clients = Number(process.env.SERVER_API_WS_CLIENTS || 10);
const durationMs = Number(process.env.SERVER_API_WS_DURATION_MS || 5000);
const apiKey = process.env.SERVER_API_KEY || '';

if (!Number.isInteger(clients) || clients < 1 || clients > 1000)
    throw new Error('SERVER_API_WS_CLIENTS must be an integer from 1 to 1000');

const options = apiKey ? { headers: { Authorization: `Bearer ${apiKey}` } } : undefined;
const sockets = [];
let opened = 0;
let errors = 0;
let messages = 0;

for (let index = 0; index < clients; ++index) {
    const socket = new WebSocket(url, options);
    sockets.push(socket);
    socket.onopen = () => {
        ++opened;
        socket.send(JSON.stringify({ type: 'subscribe', events: ['player.position', 'combat.snapshot'] }));
    };
    socket.onmessage = () => { ++messages; };
    socket.onerror = () => { ++errors; };
}

setTimeout(() => {
    for (const socket of sockets)
        socket.close();

    console.log(JSON.stringify({ clients, opened, errors, messages, durationMs }));
    if (opened !== clients || errors !== 0)
        process.exit(1);
}, durationMs);
