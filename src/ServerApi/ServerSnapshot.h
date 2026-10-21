/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef SERVER_API_SNAPSHOT_H_
#define SERVER_API_SNAPSHOT_H_

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace ServerApi
{
    struct ServerSnapshot
    {
        uint32_t realmId = 0;
        uint64_t serverTime = 0;
        uint64_t uptimeSeconds = 0;
        uint32_t playersOnline = 0;
        uint32_t botsOnline = 0;
        uint32_t activeMaps = 0;
        uint32_t activeInstances = 0;
    };

    struct CharacterStatSnapshot
    {
        std::array<uint32_t, 5> primaryAttributes{};
        uint32_t health = 0;
        uint32_t maxHealth = 0;
        std::array<uint32_t, 7> powers{};
        std::array<uint32_t, 7> maxPowers{};
        uint32_t armor = 0;
        std::array<uint32_t, 6> resistances{};
        float blockPct = 0.0f;
        float dodgePct = 0.0f;
        float parryPct = 0.0f;
        float critPct = 0.0f;
        float rangedCritPct = 0.0f;
        float spellCritPct = 0.0f;
        uint32_t attackPower = 0;
        uint32_t rangedAttackPower = 0;
        uint32_t spellPower = 0;
        uint32_t resilience = 0;
    };

    struct PlayerSnapshot
    {
        uint64_t guid = 0;
        uint32_t accountId = 0;
        std::string name;
        uint8_t level = 0;
        uint8_t playerClass = 0;
        uint8_t race = 0;
        uint32_t mapId = 0;
        uint32_t zoneId = 0;
        uint32_t health = 0;
        uint32_t maxHealth = 0;
        uint32_t power = 0;
        uint32_t maxPower = 0;
        CharacterStatSnapshot stats;
        bool bot = false;
        bool inCombat = false;
        uint64_t victimGuid = 0;
        float x = 0.0f;
        float y = 0.0f;
        float z = 0.0f;
        float orientation = 0.0f;
    };

    struct GroupSnapshot
    {
        uint64_t id = 0;
        uint64_t leaderGuid = 0;
        bool raid = false;
        std::vector<uint64_t> memberGuids;
    };

    struct InstanceSnapshot
    {
        uint32_t instanceId = 0;
        uint32_t mapId = 0;
        uint32_t difficulty = 0;
        uint32_t players = 0;
    };

    ServerSnapshot GetServerSnapshot();
    std::vector<PlayerSnapshot> GetPlayerSnapshots();
    std::vector<GroupSnapshot> GetGroupSnapshots();
    std::vector<InstanceSnapshot> GetInstanceSnapshots();
    void RefreshServerSnapshot();
}

#endif
