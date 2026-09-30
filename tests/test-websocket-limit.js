#!/usr/bin/env node

const url = process.env.SERVER_API_WS_URL || 'ws://127.0.0.1:7878/ws/v1/events';
const clients = Number(process.env.SERVER_API_WS_LIMIT_TEST_CLIENTS || 51);
const sockets = [];
let opened = 0;
let rejected = 0;

for (let index = 0; index < clients; ++index) {
    const socket = new WebSocket(url);
    sockets.push(socket);
    socket.onopen = () => {
        ++opened;
        socket.close();
    };
    socket.onerror = () => { ++rejected; };
}

setTimeout(() => {
    for (const socket of sockets)
        socket.close();

    console.log(JSON.stringify({ clients, opened, rejected }));
    if (rejected === 0)
        process.exit(1);
}, 3000);
