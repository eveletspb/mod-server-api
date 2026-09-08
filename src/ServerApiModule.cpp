/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/ApiServer.h"
#include "ServerApi/CommandQueue.h"
#include "ServerApi/EventBus.h"
#include "ServerApi/ServerApiConfig.h"
#include "ServerApi/ServerSnapshot.h"

#include "Log.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <algorithm>
#include <string>
#include <unordered_map>
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

    void LogStartupBanner(bool enabled)
    {
        LOG_INFO("server.loading", "+--------------------------------------+");
        LOG_INFO("server.loading", "|          MOD-SERVER-API              |");
        LOG_INFO("server.loading", "|   AzerothCore runtime bridge v0.1   |");
        LOG_INFO("server.loading", "+--------------------------------------+");
        LOG_INFO("server.loading", ">> Status: {}", enabled ? "enabled" : "disabled by configuration");
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
            LogStartupBanner(_config.enabled);

            if (!_config.enabled)
                return;

            ServerApi::GetEventBus().Start();
            if (!_server.Start(_config))
            {
                ServerApi::GetEventBus().Stop();
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

            ServerApi::GetCommandQueue().Drain(100);
            _snapshotTimer += diff;
            _combatTimer += diff;
            if (_snapshotTimer < _config.positionUpdatesIntervalMs)
                return;

            _snapshotTimer = 0;
            std::vector<ServerApi::PlayerSnapshot> const previousPlayers = _players;
            std::vector<ServerApi::GroupSnapshot> const previousGroups = _groups;
            std::vector<ServerApi::InstanceSnapshot> const previousInstances = _instances;
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
            for (ServerApi::PlayerSnapshot const& player : players)
            {
                auto const previous = std::find_if(previousPlayers.begin(), previousPlayers.end(),
                    [&player](ServerApi::PlayerSnapshot const& item)
                {
                    return item.guid == player.guid;
                });
                if (previous != previousPlayers.end() &&
                    (previous->mapId != player.mapId || previous->x != player.x || previous->y != player.y ||
                     previous->z != player.z || previous->orientation != player.orientation))
                    ServerApi::Publish("player.position", BuildPlayerPositionEventData(player),
                        ServerApi::EventPriority::Telemetry);
            }

            for (ServerApi::GroupSnapshot const& group : groups)
            {
                auto const previous = std::find_if(previousGroups.begin(), previousGroups.end(),
                    [&group](ServerApi::GroupSnapshot const& item)
                {
                    return item.id == group.id;
                });
                if (previous == previousGroups.end())
                    ServerApi::Publish("group.created", BuildGroupEventData(group));
                else if (previous->leaderGuid != group.leaderGuid || previous->raid != group.raid ||
                    previous->memberGuids != group.memberGuids)
                    ServerApi::Publish("group.updated", BuildGroupEventData(group));
            }

            for (ServerApi::GroupSnapshot const& group : previousGroups)
            {
                auto const current = std::find_if(groups.begin(), groups.end(),
                    [&group](ServerApi::GroupSnapshot const& item)
                {
                    return item.id == group.id;
                });
                if (current == groups.end())
                    ServerApi::Publish("group.disbanded", BuildGroupEventData(group));
            }

            for (ServerApi::InstanceSnapshot const& instance : instances)
            {
                auto const previous = std::find_if(previousInstances.begin(), previousInstances.end(),
                    [&instance](ServerApi::InstanceSnapshot const& item)
                {
                    return item.instanceId == instance.instanceId;
                });
                if (previous == previousInstances.end())
                    ServerApi::Publish("instance.started", BuildInstanceEventData(instance));
                else if (previous->mapId != instance.mapId || previous->difficulty != instance.difficulty ||
                    previous->players != instance.players)
                    ServerApi::Publish("instance.updated", BuildInstanceEventData(instance));
            }

            for (ServerApi::InstanceSnapshot const& instance : previousInstances)
            {
                auto const current = std::find_if(instances.begin(), instances.end(),
                    [&instance](ServerApi::InstanceSnapshot const& item)
                {
                    return item.instanceId == instance.instanceId;
                });
                if (current == instances.end())
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
