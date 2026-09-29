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

#include <memory>
#include <mutex>
#include <unordered_set>

namespace ServerApi
{
    namespace
    {
        std::mutex SnapshotMutex;
        ServerSnapshot Snapshot;
        auto Players = std::make_shared<std::vector<PlayerSnapshot> const>();
        auto Groups = std::make_shared<std::vector<GroupSnapshot> const>();
        auto Instances = std::make_shared<std::vector<InstanceSnapshot> const>();
    }

    ServerSnapshot GetServerSnapshot()
    {
        std::lock_guard lock(SnapshotMutex);
        return Snapshot;
    }

    std::vector<PlayerSnapshot> GetPlayerSnapshots()
    {
        std::shared_ptr<std::vector<PlayerSnapshot> const> players;
        {
            std::lock_guard lock(SnapshotMutex);
            players = Players;
        }
        return *players;
    }

    std::vector<GroupSnapshot> GetGroupSnapshots()
    {
        std::shared_ptr<std::vector<GroupSnapshot> const> groups;
        {
            std::lock_guard lock(SnapshotMutex);
            groups = Groups;
        }
        return *groups;
    }

    std::vector<InstanceSnapshot> GetInstanceSnapshots()
    {
        std::shared_ptr<std::vector<InstanceSnapshot> const> instances;
        {
            std::lock_guard lock(SnapshotMutex);
            instances = Instances;
        }
        return *instances;
    }

    void RefreshServerSnapshot()
    {
        ServerSnapshot updated;
        updated.realmId = realm.Id.Realm;
        updated.serverTime = static_cast<uint64_t>(GameTime::GetGameTime().count());
        updated.uptimeSeconds = static_cast<uint64_t>(GameTime::GetUptime().count());
        updated.playersOnline = sWorldSessionMgr->GetPlayerCount();

        std::vector<InstanceSnapshot> updatedInstances;
        sMapMgr->DoForAllMaps([&updated, &updatedInstances](Map* map)
        {
            ++updated.activeMaps;
            if (map->GetInstanceId() == 0 || !map->Instanceable())
                return;

            if (map->IsDungeon())
            {
                ++updated.activeInstances;
                updatedInstances.push_back({
                    map->GetInstanceId(),
                    map->GetId(),
                    static_cast<uint32_t>(map->GetDifficulty()),
                    map->GetPlayersCountExceptGMs()
                });
            }
            else if (map->IsBattleground() || map->IsBattleArena())
                ++updated.activeInstances;
        });

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

        auto nextPlayers = std::make_shared<std::vector<PlayerSnapshot> const>(std::move(updatedPlayers));
        auto nextGroups = std::make_shared<std::vector<GroupSnapshot> const>(std::move(updatedGroups));
        auto nextInstances = std::make_shared<std::vector<InstanceSnapshot> const>(std::move(updatedInstances));
        std::shared_ptr<std::vector<PlayerSnapshot> const> oldPlayers;
        std::shared_ptr<std::vector<GroupSnapshot> const> oldGroups;
        std::shared_ptr<std::vector<InstanceSnapshot> const> oldInstances;
        {
            std::lock_guard lock(SnapshotMutex);
            Snapshot = updated;
            oldPlayers = std::move(Players);
            oldGroups = std::move(Groups);
            oldInstances = std::move(Instances);
            Players = std::move(nextPlayers);
            Groups = std::move(nextGroups);
            Instances = std::move(nextInstances);
        }
    }
}
