/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef SERVER_API_CONFIG_H_
#define SERVER_API_CONFIG_H_

#include <cstdint>
#include <string>

namespace ServerApi
{
    struct Config
    {
        bool enabled = true;
        std::string bindAddress = "127.0.0.1";
        uint16_t port = 7878;
        uint32_t maxRequestBytes = 1024 * 1024;
        uint32_t maxRequestsPerSecond = 1000;
        std::string authProvider = "none";
        bool webSocketEnabled = true;
        bool sseEnabled = true;
        uint32_t maxSseSubscriptions = 20;
        uint32_t maxSseQueue = 256;
        uint32_t maxSseQueueBytes = 2 * 1024 * 1024;
        uint32_t maxSseClients = 10;
        uint32_t sseHeartbeatSeconds = 15;
        uint32_t maxWebSocketFrameBytes = 16 * 1024;
        uint32_t maxWebSocketSubscriptions = 20;
        uint32_t maxWebSocketQueue = 256;
        uint32_t maxWebSocketQueueBytes = 2 * 1024 * 1024;
        uint32_t maxWebSocketClients = 10;
        uint32_t maxWebSocketMessagesPerSecond = 10000;
        uint32_t maxWebSocketPublishQueue = 10000;
        uint32_t positionUpdatesIntervalMs = 1000;
        uint32_t combatSnapshotIntervalMs = 2000;
    };

    Config LoadConfig();
}

#endif
