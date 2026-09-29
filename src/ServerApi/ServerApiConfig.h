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
        bool authEnabled = false;
        std::string apiKey;
        bool webSocketEnabled = true;
        uint32_t maxWebSocketFrameBytes = 1024 * 1024;
        uint32_t maxWebSocketSubscriptions = 100;
        uint32_t maxWebSocketQueue = 100;
        uint32_t maxWebSocketClients = 50;
        uint32_t positionUpdatesIntervalMs = 1000;
        uint32_t combatSnapshotIntervalMs = 2000;
    };

    Config LoadConfig();
}

#endif
