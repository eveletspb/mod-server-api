/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/ServerSnapshot.h"

#include "GameTime.h"
#include "Group.h"
#include "MapMgr.h"
#include "Player.h"
#include "Realm.h"
#include "WorldSessionMgr.h"

#include <mutex>
#include <unordered_set>

namespace ServerApi
{
    namespace
    {
        std::mutex SnapshotMutex;
        ServerSnapshot Snapshot;
        std::vector<PlayerSnapshot> Players;
        std::vector<GroupSnapshot> Groups;
        std::vector<InstanceSnapshot> Instances;
    }

    ServerSnapshot GetServerSnapshot()
    {
        std::lock_guard lock(SnapshotMutex);
        return Snapshot;
    }

    std::vector<PlayerSnapshot> GetPlayerSnapshots()
    {
        std::lock_guard lock(SnapshotMutex);
        return Players;
    }

    std::vector<GroupSnapshot> GetGroupSnapshots()
    {
        std::lock_guard lock(SnapshotMutex);
        return Groups;
    }

    std::vector<InstanceSnapshot> GetInstanceSnapshots()
    {
        std::lock_guard lock(SnapshotMutex);
        return Instances;
    }

    void RefreshServerSnapshot()
    {
        ServerSnapshot updated;
        updated.realmId = realm.Id.Realm;
        updated.serverTime = static_cast<uint64_t>(GameTime::GetGameTime().count());
        updated.uptimeSeconds = static_cast<uint64_t>(GameTime::GetUptime().count());
        updated.playersOnline = sWorldSessionMgr->GetPlayerCount();

        sMapMgr->DoForAllMaps([&updated](Map*)
        {
            ++updated.activeMaps;
        });

        uint32_t dungeons = 0;
        uint32_t battlegrounds = 0;
        uint32_t arenas = 0;
        sMapMgr->GetNumInstances(dungeons, battlegrounds, arenas);
        updated.activeInstances = dungeons + battlegrounds + arenas;

        std::vector<PlayerSnapshot> updatedPlayers;
        std::vector<GroupSnapshot> updatedGroups;
        std::unordered_set<uint64_t> groupIds;
        updatedPlayers.reserve(updated.playersOnline);
        sWorldSessionMgr->DoForAllOnlinePlayers([&updated, &updatedPlayers, &updatedGroups, &groupIds](Player* player)
        {
            PlayerSnapshot playerSnapshot;
            playerSnapshot.guid = player->GetGUID().GetCounter();
            playerSnapshot.name = player->GetName();
            playerSnapshot.level = player->GetLevel();
            playerSnapshot.playerClass = player->getClass();
            playerSnapshot.race = player->getRace();
            playerSnapshot.mapId = player->GetMapId();
            playerSnapshot.zoneId = player->GetZoneId();
            playerSnapshot.health = player->GetHealth();
            playerSnapshot.maxHealth = player->GetMaxHealth();
            Powers const powerType = player->getPowerType();
            playerSnapshot.power = player->GetPower(powerType);
            playerSnapshot.maxPower = player->GetMaxPower(powerType);
            if (WorldSession* session = player->GetSession())
            {
                playerSnapshot.accountId = session->GetAccountId();
                playerSnapshot.remoteAddress = session->GetRemoteAddress();
                playerSnapshot.latency = session->GetLatency();
                playerSnapshot.bot = session->IsBot();
            }
            if (playerSnapshot.bot)
                ++updated.botsOnline;
            playerSnapshot.inCombat = player->IsInCombat();
            if (Unit* victim = player->GetVictim())
                playerSnapshot.victimGuid = victim->GetGUID().GetCounter();
            playerSnapshot.x = player->GetPositionX();
            playerSnapshot.y = player->GetPositionY();
            playerSnapshot.z = player->GetPositionZ();
            playerSnapshot.orientation = player->GetOrientation();
            updatedPlayers.push_back(std::move(playerSnapshot));

            Group* group = player->GetGroup();
            if (!group || !groupIds.insert(group->GetGUID().GetCounter()).second)
                return;

            GroupSnapshot groupSnapshot;
            groupSnapshot.id = group->GetGUID().GetCounter();
            groupSnapshot.leaderGuid = group->GetLeaderGUID().GetCounter();
            groupSnapshot.raid = group->isRaidGroup();
            for (Group::MemberSlot const& member : group->GetMemberSlots())
                groupSnapshot.memberGuids.push_back(member.guid.GetCounter());
            updatedGroups.push_back(std::move(groupSnapshot));
        });

        std::vector<InstanceSnapshot> updatedInstances;
        sMapMgr->DoForAllMaps([&updatedInstances](Map* map)
        {
            if (map->GetInstanceId() == 0 || !map->Instanceable() || !map->IsDungeon())
                return;

            updatedInstances.push_back({
                map->GetInstanceId(),
                map->GetId(),
                static_cast<uint32_t>(map->GetDifficulty()),
                map->GetPlayersCountExceptGMs()
            });
        });

        std::lock_guard lock(SnapshotMutex);
        Snapshot = updated;
        Players = std::move(updatedPlayers);
        Groups = std::move(updatedGroups);
        Instances = std::move(updatedInstances);
    }
}
