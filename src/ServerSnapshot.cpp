/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/ServerSnapshot.h"

#include "Configuration/Config.h"
#include "GameTime.h"
#include "Group.h"
#include "MapMgr.h"
#include "Player.h"
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
        updated.realmId = sConfigMgr->GetOption<uint32_t>("RealmID", 0);
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
        sWorldSessionMgr->DoForAllOnlinePlayers([&updated, &updatedPlayers](Player* player)
        {
            PlayerSnapshot snapshot;
            snapshot.guid = player->GetGUID().GetCounter();
            snapshot.name = player->GetName();
            snapshot.level = player->GetLevel();
            snapshot.playerClass = player->getClass();
            snapshot.race = player->getRace();
            snapshot.mapId = player->GetMapId();
            snapshot.zoneId = player->GetZoneId();
            snapshot.health = player->GetHealth();
            snapshot.maxHealth = player->GetMaxHealth();
            snapshot.power = player->GetPower(POWER_MANA);
            snapshot.maxPower = player->GetMaxPower(POWER_MANA);
            snapshot.bot = player->GetSession() && player->GetSession()->IsBot();
            if (snapshot.bot)
                ++updated.botsOnline;
            snapshot.inCombat = player->IsInCombat();
            if (Unit* victim = player->GetVictim())
                snapshot.victimGuid = victim->GetGUID().GetCounter();
            snapshot.x = player->GetPositionX();
            snapshot.y = player->GetPositionY();
            snapshot.z = player->GetPositionZ();
            snapshot.orientation = player->GetOrientation();
            updatedPlayers.push_back(std::move(snapshot));
        });

        sWorldSessionMgr->DoForAllOnlinePlayers([&updatedGroups, &groupIds](Player* player)
        {
            Group* group = player->GetGroup();
            if (!group || !groupIds.insert(group->GetGUID().GetCounter()).second)
                return;

            GroupSnapshot snapshot;
            snapshot.id = group->GetGUID().GetCounter();
            snapshot.leaderGuid = group->GetLeaderGUID().GetCounter();
            snapshot.raid = group->isRaidGroup();
            for (Group::MemberSlot const& member : group->GetMemberSlots())
                snapshot.memberGuids.push_back(member.guid.GetCounter());
            updatedGroups.push_back(std::move(snapshot));
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
