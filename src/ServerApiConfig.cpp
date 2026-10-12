/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/ServerApiConfig.h"

#include "Configuration/Config.h"
#include "Log.h"

namespace ServerApi
{
    Config LoadConfig()
    {
        Config config;
        config.enabled = sConfigMgr->GetOption<bool>("ServerApi.Enable", true);
        config.bindAddress = sConfigMgr->GetOption<std::string>("ServerApi.BindAddress", "127.0.0.1");
        config.port = sConfigMgr->GetOption<uint16_t>("ServerApi.Port", 7878);
        config.maxRequestBytes = sConfigMgr->GetOption<uint32_t>("ServerApi.MaxRequestBytes", 1024 * 1024);
        config.maxRequestsPerSecond = sConfigMgr->GetOption<uint32_t>("ServerApi.MaxRequestsPerSecond", 1000);
        config.ioThreads = sConfigMgr->GetOption<uint32_t>("ServerApi.IOThreads", 4);
        config.authProvider = sConfigMgr->GetOption<std::string>("ServerApi.Auth.Provider", "none");
        config.webSocketEnabled = sConfigMgr->GetOption<bool>("ServerApi.WebSocket.Enable", true);
        config.sseEnabled = sConfigMgr->GetOption<bool>("ServerApi.SSE.Enable", true);
        config.maxSseSubscriptions = sConfigMgr->GetOption<uint32_t>("ServerApi.SSE.MaxSubscriptions", 20);
        config.maxSseQueue = sConfigMgr->GetOption<uint32_t>("ServerApi.SSE.MaxQueue", 256);
        config.maxSseQueueBytes = sConfigMgr->GetOption<uint32_t>("ServerApi.SSE.MaxQueueBytes", 2 * 1024 * 1024);
        config.maxSseClients = sConfigMgr->GetOption<uint32_t>("ServerApi.SSE.MaxClients", 10);
        config.sseHeartbeatSeconds = sConfigMgr->GetOption<uint32_t>("ServerApi.SSE.HeartbeatSeconds", 15);
        config.maxWebSocketFrameBytes =
            sConfigMgr->GetOption<uint32_t>("ServerApi.WebSocket.MaxFrameBytes", 16 * 1024);
        config.maxWebSocketSubscriptions = sConfigMgr->GetOption<uint32_t>(
            "ServerApi.WebSocket.MaxSubscriptions", 20);
        config.maxWebSocketQueue = sConfigMgr->GetOption<uint32_t>("ServerApi.WebSocket.MaxQueue", 256);
        config.maxWebSocketQueueBytes = sConfigMgr->GetOption<uint32_t>(
            "ServerApi.WebSocket.MaxQueueBytes", 2 * 1024 * 1024);
        config.maxWebSocketClients = sConfigMgr->GetOption<uint32_t>("ServerApi.WebSocket.MaxClients", 10);
        config.maxWebSocketMessagesPerSecond = sConfigMgr->GetOption<uint32_t>(
            "ServerApi.WebSocket.MaxMessagesPerSecond", 10000);
        config.maxWebSocketPublishQueue = sConfigMgr->GetOption<uint32_t>(
            "ServerApi.WebSocket.MaxPublishQueue", 10000);
        config.positionUpdatesIntervalMs =
            sConfigMgr->GetOption<uint32_t>("ServerApi.PositionUpdates.IntervalMs", 1000);
        config.combatSnapshotIntervalMs = sConfigMgr->GetOption<uint32_t>("ServerApi.CombatSnapshot.IntervalMs", 2000);

        if (config.positionUpdatesIntervalMs == 0)
        {
            LOG_ERROR("server-api.config", "ServerApi.PositionUpdates.IntervalMs must be greater than zero");
            config.enabled = false;
        }

        if (config.combatSnapshotIntervalMs == 0)
        {
            LOG_ERROR("server-api.config", "ServerApi.CombatSnapshot.IntervalMs must be greater than zero");
            config.enabled = false;
        }

        if (config.maxRequestBytes == 0)
        {
            LOG_ERROR("server-api.config", "ServerApi.MaxRequestBytes must be greater than zero");
            config.enabled = false;
        }

        if (config.ioThreads == 0 || config.ioThreads > 32)
        {
            LOG_ERROR("server-api.config", "ServerApi.IOThreads must be between 1 and 32");
            config.enabled = false;
        }

        if (config.webSocketEnabled && (config.maxWebSocketFrameBytes == 0 ||
            config.maxWebSocketSubscriptions == 0 || config.maxWebSocketQueue == 0 ||
            config.maxWebSocketQueueBytes == 0 ||
            config.maxWebSocketClients == 0 || config.maxWebSocketMessagesPerSecond == 0 ||
            config.maxWebSocketPublishQueue == 0))
        {
            LOG_ERROR("server-api.config", "WebSocket limits must be greater than zero");
            config.enabled = false;
        }

        if (config.sseEnabled && (config.maxSseSubscriptions == 0 || config.maxSseQueue == 0 ||
            config.maxSseQueueBytes == 0 || config.maxSseClients == 0 || config.sseHeartbeatSeconds == 0))
        {
            LOG_ERROR("server-api.config", "SSE limits and heartbeat must be greater than zero");
            config.enabled = false;
        }

        if (config.authProvider.empty())
        {
            LOG_ERROR("server-api.config", "ServerApi.Auth.Provider must not be empty");
            config.enabled = false;
        }

        return config;
    }
}
