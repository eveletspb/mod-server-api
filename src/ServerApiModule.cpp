/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/ApiServer.h"
#include "ServerApi/CommandQueue.h"
#include "ServerApi/EventBus.h"
#include "ServerApi/ModuleRegistry.h"
#include "ServerApi/ServerApiConfig.h"
#include "ServerApi/ServerSnapshot.h"

#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    std::unordered_map<std::string, std::string> BuildPlayerEventData(Player* player)
    {
        return {
            {"guid", std::to_string(player->GetGUID().GetCounter())},
            {"name", player->GetName()},
            {"level", std::to_string(player->GetLevel())},
            {"mapId", std::to_string(player->GetMapId())},
            {"zoneId", std::to_string(player->GetZoneId())}
        };
    }

    std::unordered_map<std::string, std::string> BuildGroupEventData(ServerApi::GroupSnapshot const& group)
    {
        return {
            {"id", std::to_string(group.id)},
            {"leaderGuid", std::to_string(group.leaderGuid)},
            {"raid", group.raid ? "true" : "false"},
            {"members", std::to_string(group.memberGuids.size())}
        };
    }

    std::unordered_map<std::string, std::string> BuildPlayerPositionEventData(ServerApi::PlayerSnapshot const& player)
    {
        return {
            {"guid", std::to_string(player.guid)},
            {"mapId", std::to_string(player.mapId)},
            {"x", std::to_string(player.x)},
            {"y", std::to_string(player.y)},
            {"z", std::to_string(player.z)},
            {"orientation", std::to_string(player.orientation)}
        };
    }

    std::unordered_map<std::string, std::string> BuildCombatSnapshotEventData(ServerApi::PlayerSnapshot const& player)
    {
        return {
            {"guid", std::to_string(player.guid)},
            {"mapId", std::to_string(player.mapId)},
            {"victimGuid", std::to_string(player.victimGuid)},
            {"health", std::to_string(player.health)},
            {"maxHealth", std::to_string(player.maxHealth)},
            {"power", std::to_string(player.power)},
            {"maxPower", std::to_string(player.maxPower)}
        };
    }

    std::unordered_map<std::string, std::string> BuildInstanceEventData(ServerApi::InstanceSnapshot const& instance)
    {
        return {
            {"instanceId", std::to_string(instance.instanceId)},
            {"mapId", std::to_string(instance.mapId)},
            {"difficulty", std::to_string(instance.difficulty)},
            {"players", std::to_string(instance.players)}
        };
    }

    void LogStartupBanner(ServerApi::Config const& config)
    {
        LOG_INFO("server.loading", "+--------------------------------------+");
        LOG_INFO("server.loading", "|          MOD-SERVER-API              |");
        LOG_INFO("server.loading", "|   AzerothCore runtime bridge v0.1   |");
        LOG_INFO("server.loading", "+--------------------------------------+");
        LOG_INFO("server.loading", ">> Status: {}", config.enabled ? "enabled" : "disabled by configuration");
        LOG_INFO("server.loading", ">> Listener: {}:{}", config.bindAddress, config.port);
        LOG_INFO("server.loading", ">> Authentication: {}", config.authEnabled ? "Bearer enabled" : "disabled");
        LOG_INFO("server.loading", ">> HTTP limits: request={} bytes, rate={}/s (0 = unlimited)",
            config.maxRequestBytes, config.maxRequestsPerSecond);
        LOG_INFO("server.loading", ">> WebSocket: {}", config.webSocketEnabled ? "enabled" : "disabled");
        if (config.webSocketEnabled)
            LOG_INFO("server.loading", ">> WebSocket limits: frame={} bytes, subscriptions={}, queue={}, clients={}",
                config.maxWebSocketFrameBytes, config.maxWebSocketSubscriptions, config.maxWebSocketQueue,
                config.maxWebSocketClients);

        LOG_INFO("server.loading", ">> Sampling: position={} ms, combat={} ms",
            config.positionUpdatesIntervalMs, config.combatSnapshotIntervalMs);
    }

    class ServerApiWorldScript : public WorldScript
    {
    public:
        ServerApiWorldScript() : WorldScript("ServerApiWorldScript", {
            WORLDHOOK_ON_BEFORE_CONFIG_LOAD,
            WORLDHOOK_ON_UPDATE,
            WORLDHOOK_ON_STARTUP,
            WORLDHOOK_ON_SHUTDOWN
        }) { }

        void OnBeforeConfigLoad(bool reload) override
        {
            ServerApi::Config const loadedConfig = ServerApi::LoadConfig();
            if (!reload || !_started)
            {
                _config = loadedConfig;
                return;
            }

            if (loadedConfig.positionUpdatesIntervalMs > 0 && loadedConfig.combatSnapshotIntervalMs > 0)
            {
                _config.positionUpdatesIntervalMs = loadedConfig.positionUpdatesIntervalMs;
                _config.combatSnapshotIntervalMs = loadedConfig.combatSnapshotIntervalMs;
            }

            LOG_INFO("server-api.config",
                "Sampling intervals reloaded; listener, authentication and limit changes require restart");
        }

        void OnStartup() override
        {
            LogStartupBanner(_config);

            if (!_config.enabled)
                return;

            ServerApi::GetModuleRegistry().Register({
                "server-api", "0.1.0", {"server", "runtime", "commands", "events"}});

            ServerApi::GetEventBus().Start();
            if (!_server.Start(_config))
            {
                ServerApi::GetEventBus().Stop();
                ServerApi::GetModuleRegistry().Unregister("server-api");
                return;
            }

            ServerApi::RefreshServerSnapshot();
            _groups = ServerApi::GetGroupSnapshots();
            _instances = ServerApi::GetInstanceSnapshots();
            _players = ServerApi::GetPlayerSnapshots();
            PublishSnapshotEvents({}, _players, {}, _groups, {}, _instances);
            _started = true;
            ServerApi::Publish("server.started");
        }

        void OnUpdate(uint32 diff) override
        {
            if (!_started)
                return;

            ServerApi::GetCommandQueue().Drain(100, std::chrono::milliseconds(2));
            _snapshotTimer += diff;
            _combatTimer += diff;
            if (_snapshotTimer < _config.positionUpdatesIntervalMs)
                return;

            _snapshotTimer = 0;
            std::vector<ServerApi::PlayerSnapshot> const previousPlayers = std::move(_players);
            std::vector<ServerApi::GroupSnapshot> const previousGroups = std::move(_groups);
            std::vector<ServerApi::InstanceSnapshot> const previousInstances = std::move(_instances);
            ServerApi::RefreshServerSnapshot();
            _players = ServerApi::GetPlayerSnapshots();
            _groups = ServerApi::GetGroupSnapshots();
            _instances = ServerApi::GetInstanceSnapshots();
            PublishSnapshotEvents(previousPlayers, _players, previousGroups, _groups, previousInstances, _instances);
            if (_combatTimer >= _config.combatSnapshotIntervalMs)
            {
                _combatTimer = 0;
                for (ServerApi::PlayerSnapshot const& player : _players)
                {
                    if (player.inCombat)
                        ServerApi::Publish("combat.snapshot", BuildCombatSnapshotEventData(player),
                            ServerApi::EventPriority::Telemetry);
                }
            }
        }

        void OnShutdown() override
        {
            if (!_started)
                return;

            ServerApi::Publish("server.stopping");
            _server.Stop();
            ServerApi::GetCommandQueue().Clear();
            ServerApi::GetEventBus().Stop();
            ServerApi::GetModuleRegistry().Unregister("server-api");
            _started = false;
            _groups.clear();
            _instances.clear();
            _players.clear();
            _snapshotTimer = 0;
            _combatTimer = 0;
        }

    private:
        static void PublishSnapshotEvents(std::vector<ServerApi::PlayerSnapshot> const& previousPlayers,
            std::vector<ServerApi::PlayerSnapshot> const& players,
            std::vector<ServerApi::GroupSnapshot> const& previousGroups,
            std::vector<ServerApi::GroupSnapshot> const& groups,
            std::vector<ServerApi::InstanceSnapshot> const& previousInstances,
            std::vector<ServerApi::InstanceSnapshot> const& instances)
        {
            std::unordered_map<uint64_t, ServerApi::PlayerSnapshot const*> previousPlayersByGuid;
            previousPlayersByGuid.reserve(previousPlayers.size());
            for (ServerApi::PlayerSnapshot const& player : previousPlayers)
                previousPlayersByGuid.emplace(player.guid, &player);

            std::unordered_map<uint64_t, ServerApi::GroupSnapshot const*> previousGroupsById;
            previousGroupsById.reserve(previousGroups.size());
            for (ServerApi::GroupSnapshot const& group : previousGroups)
                previousGroupsById.emplace(group.id, &group);

            std::unordered_map<uint64_t, ServerApi::GroupSnapshot const*> groupsById;
            groupsById.reserve(groups.size());
            for (ServerApi::GroupSnapshot const& group : groups)
                groupsById.emplace(group.id, &group);

            std::unordered_map<uint32_t, ServerApi::InstanceSnapshot const*> previousInstancesById;
            previousInstancesById.reserve(previousInstances.size());
            for (ServerApi::InstanceSnapshot const& instance : previousInstances)
                previousInstancesById.emplace(instance.instanceId, &instance);

            std::unordered_map<uint32_t, ServerApi::InstanceSnapshot const*> instancesById;
            instancesById.reserve(instances.size());
            for (ServerApi::InstanceSnapshot const& instance : instances)
                instancesById.emplace(instance.instanceId, &instance);

            for (ServerApi::PlayerSnapshot const& player : players)
            {
                auto const previous = previousPlayersByGuid.find(player.guid);
                if (previous != previousPlayersByGuid.end() &&
                    (previous->second->mapId != player.mapId || previous->second->x != player.x ||
                     previous->second->y != player.y || previous->second->z != player.z ||
                     previous->second->orientation != player.orientation))
                    ServerApi::Publish("player.position", BuildPlayerPositionEventData(player),
                        ServerApi::EventPriority::Telemetry);
            }

            for (ServerApi::GroupSnapshot const& group : groups)
            {
                auto const previous = previousGroupsById.find(group.id);
                if (previous == previousGroupsById.end())
                    ServerApi::Publish("group.created", BuildGroupEventData(group));
                else if (previous->second->leaderGuid != group.leaderGuid || previous->second->raid != group.raid ||
                    previous->second->memberGuids != group.memberGuids)
                    ServerApi::Publish("group.updated", BuildGroupEventData(group));
            }

            for (ServerApi::GroupSnapshot const& group : previousGroups)
            {
                if (!groupsById.contains(group.id))
                    ServerApi::Publish("group.disbanded", BuildGroupEventData(group));
            }

            for (ServerApi::InstanceSnapshot const& instance : instances)
            {
                auto const previous = previousInstancesById.find(instance.instanceId);
                if (previous == previousInstancesById.end())
                    ServerApi::Publish("instance.started", BuildInstanceEventData(instance));
                else if (previous->second->mapId != instance.mapId ||
                    previous->second->difficulty != instance.difficulty || previous->second->players != instance.players)
                    ServerApi::Publish("instance.updated", BuildInstanceEventData(instance));
            }

            for (ServerApi::InstanceSnapshot const& instance : previousInstances)
            {
                if (!instancesById.contains(instance.instanceId))
                    ServerApi::Publish("instance.stopped", BuildInstanceEventData(instance));
            }
        }

        ServerApi::Config _config;
        ServerApi::ApiServer _server;
        bool _started = false;
        uint32 _snapshotTimer = 0;
        uint32 _combatTimer = 0;
        std::vector<ServerApi::PlayerSnapshot> _players;
        std::vector<ServerApi::GroupSnapshot> _groups;
        std::vector<ServerApi::InstanceSnapshot> _instances;
    };

    class ServerApiPlayerScript : public PlayerScript
    {
    public:
        ServerApiPlayerScript() : PlayerScript("ServerApiPlayerScript", {
            PLAYERHOOK_ON_LOGIN,
            PLAYERHOOK_ON_LOGOUT,
            PLAYERHOOK_ON_PLAYER_JUST_DIED
        }) { }

        void OnPlayerLogin(Player* player) override
        {
            ServerApi::Publish("player.login", BuildPlayerEventData(player));
        }

        void OnPlayerLogout(Player* player) override
        {
            ServerApi::Publish("player.logout", BuildPlayerEventData(player));
        }

        void OnPlayerJustDied(Player* player) override
        {
            ServerApi::Publish("player.death", BuildPlayerEventData(player));
        }
    };
}

void AddServerApiScripts()
{
    LOG_INFO("server.loading", ">> Loading mod-server-api");
    new ServerApiWorldScript();
    new ServerApiPlayerScript();
}
