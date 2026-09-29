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
        config.authEnabled = sConfigMgr->GetOption<bool>("ServerApi.Auth.Enable", false);
        config.apiKey = sConfigMgr->GetOption<std::string>("ServerApi.Auth.ApiKey", "");
        config.webSocketEnabled = sConfigMgr->GetOption<bool>("ServerApi.WebSocket.Enable", true);
        config.maxWebSocketFrameBytes =
            sConfigMgr->GetOption<uint32_t>("ServerApi.WebSocket.MaxFrameBytes", 1024 * 1024);
        config.maxWebSocketSubscriptions = sConfigMgr->GetOption<uint32_t>("ServerApi.WebSocket.MaxSubscriptions", 100);
        config.maxWebSocketQueue = sConfigMgr->GetOption<uint32_t>("ServerApi.WebSocket.MaxQueue", 100);
        config.maxWebSocketClients = sConfigMgr->GetOption<uint32_t>("ServerApi.WebSocket.MaxClients", 50);
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

        if (config.webSocketEnabled && (config.maxWebSocketFrameBytes == 0 ||
            config.maxWebSocketSubscriptions == 0 || config.maxWebSocketQueue == 0 ||
            config.maxWebSocketClients == 0))
        {
            LOG_ERROR("server-api.config",
                "WebSocket frame, subscription, queue and client limits must be greater than zero");
            config.enabled = false;
        }

        if (config.bindAddress != "127.0.0.1" && config.bindAddress != "::1")
        {
            if (!config.authEnabled || config.apiKey.empty())
            {
                LOG_ERROR("server-api.config",
                    "Refusing non-local bind {} without ServerApi.Auth.Enable and a non-empty API key",
                    config.bindAddress);
                config.enabled = false;
            }
        }

        if (config.enabled && config.authEnabled && config.apiKey.empty())
        {
            LOG_ERROR("server-api.config", "ServerApi.Auth.ApiKey must be configured when the API is enabled");
            config.enabled = false;
        }

        return config;
    }
}
