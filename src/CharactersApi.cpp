/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/CharactersApi.h"

#include "ServerApi/CommandQueue.h"
#include "ServerApi/HttpUtils.h"
#include "ServerApi/ServerSnapshot.h"

#include "Common.h"
#include "DatabaseEnv.h"
#include "DataStores/DBCStores.h"
#include "Field.h"
#include "Instances/InstanceSaveMgr.h"
#include "Log.h"
#include "Map.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "QueryResult.h"
#include "Spells/SpellInfo.h"
#include "Spells/SpellMgr.h"
#include "TC9Sidecar/TC9Sidecar.h"

#ifdef MOD_PLAYERBOTS
#include "Db/PlayerbotsDatabase.h"
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <boost/system/error_code.hpp>

namespace ServerApi
{
    namespace
    {
        constexpr uint32_t MaxConcurrentRequests = 32;
        constexpr uint32_t ListBatchSize = 500;
        constexpr uint32_t MaxRowsScannedPerRequest = 10000;
        constexpr auto CallbackPollInterval = std::chrono::milliseconds(10);
        constexpr auto BotTypeRefreshInterval = std::chrono::seconds(30);
        constexpr auto BotTypeRetryInterval = std::chrono::seconds(5);

        struct CharacterFilters
        {
            uint32_t limit = 100;
            uint32_t cursor = 0;
            std::string namePrefix;
            std::optional<uint32_t> accountId;
            std::vector<uint32_t> guids;
            std::vector<std::string> exactNames;
            std::optional<bool> online;
            std::optional<bool> bot;
            std::optional<std::string> botType;
            std::optional<uint8_t> race;
            std::optional<uint8_t> playerClass;
            std::optional<uint8_t> minLevel;
            std::optional<uint8_t> maxLevel;
            std::optional<uint32_t> guildId;
            std::optional<uint32_t> mapId;
            std::optional<uint32_t> zoneId;
            bool includeDeleted = false;
            bool includeTalents = false;
        };

        struct CharacterRecord
        {
            uint32_t guid = 0;
            uint32_t accountId = 0;
            std::string name;
            uint8_t race = 0;
            uint8_t playerClass = 0;
            uint8_t gender = 0;
            uint8_t level = 0;
            uint32_t xp = 0;
            uint32_t money = 0;
            uint32_t guildId = 0;
            std::string guildName;
            uint32_t mapId = 0;
            uint32_t zoneId = 0;
            bool deleted = false;
            float x = 0.0f;
            float y = 0.0f;
            float z = 0.0f;
            float orientation = 0.0f;
            uint8_t activeTalentGroup = 0;
            uint8_t talentGroupsCount = 1;
            bool available = false;
            bool hasTalentSpells = false;
            std::vector<uint32_t> talentSpells;
            bool hasTalentSummary = false;
            std::string spec;
            std::array<uint32_t, 3> talentPoints{};
            std::vector<std::string_view> suggestedRoles;
        };

        struct EquipmentStat
        {
            uint8_t type = 0;
            int32_t value = 0;
        };

        struct EquipmentDamage
        {
            float min = 0.0f;
            float max = 0.0f;
            uint8_t type = 0;
        };

        struct EquipmentEnchantmentProc
        {
            uint32_t customChance = 0;
            float ppmChance = 0.0f;
            uint32_t procEx = 0;
            uint32_t attributeMask = 0;
        };

        EquipmentEnchantmentProc BuildEnchantmentProc(SpellEnchantProcEntry const& source)
        {
            return {source.customChance, source.PPMChance, source.procEx, source.attributeMask};
        }

        void AppendEnchantmentProcJson(std::string& body, bool hasProc, EquipmentEnchantmentProc const& proc)
        {
            if (!hasProc)
            {
                body += "null";
                return;
            }

            body += "{\"customChance\":" + std::to_string(proc.customChance) +
                ",\"ppmChance\":" + std::to_string(proc.ppmChance) +
                ",\"procEx\":" + std::to_string(proc.procEx) +
                ",\"attributeMask\":" + std::to_string(proc.attributeMask) + "}";
        }

        struct EquipmentEnchantment
        {
            uint8_t slot = 0;
            uint32_t id = 0;
            uint32_t durationMs = 0;
            uint32_t charges = 0;
            std::string name;
            std::string nameRu;
            uint32_t gemId = 0;
            struct Effect
            {
                uint32_t type = 0;
                uint32_t amount = 0;
                uint32_t spellId = 0;
            };
            std::vector<Effect> effects;
            bool hasProc = false;
            EquipmentEnchantmentProc proc;
        };

        struct EquipmentSocket
        {
            uint32_t color = 0;
            uint32_t content = 0;
        };

        struct EquipmentSpellEffect
        {
            uint8_t index = 0;
            uint32_t type = 0;
            uint32_t auraType = 0;
            int32_t basePoints = 0;
            int32_t dieSides = 0;
            float realPointsPerLevel = 0.0f;
            uint32_t triggerSpell = 0;
            int32_t miscValue = 0;
            int32_t miscValueB = 0;
            uint32_t amplitudeMs = 0;
        };

        struct EquipmentSpellProc
        {
            uint32_t schoolMask = 0;
            uint32_t spellFamilyName = 0;
            std::array<uint32_t, 3> spellFamilyMask{};
            uint32_t procFlags = 0;
            uint32_t spellTypeMask = 0;
            uint32_t spellPhaseMask = 0;
            uint32_t hitMask = 0;
            uint32_t attributesMask = 0;
            uint32_t disableEffectsMask = 0;
            float procsPerMinute = 0.0f;
            float chance = 0.0f;
            int64_t cooldownMs = 0;
            uint32_t charges = 0;
        };

        std::array<uint32_t, 3> ReadFlag96(flag96 const& source)
        {
            std::array<uint32_t, 3> parts{};
            for (uint32_t bit = 0; bit < 32; ++bit)
            {
                uint32_t const bitMask = uint32_t{1} << bit;
                if (source.HasFlag(bitMask, 0, 0))
                    parts[0] |= bitMask;
                if (source.HasFlag(0, bitMask, 0))
                    parts[1] |= bitMask;
                if (source.HasFlag(0, 0, bitMask))
                    parts[2] |= bitMask;
            }
            return parts;
        }

        EquipmentSpellProc BuildSpellProc(SpellProcEntry const& source)
        {
            return {
                source.SchoolMask,
                source.SpellFamilyName,
                ReadFlag96(source.SpellFamilyMask),
                source.ProcFlags,
                source.SpellTypeMask,
                source.SpellPhaseMask,
                source.HitMask,
                source.AttributesMask,
                source.DisableEffectsMask,
                source.ProcsPerMinute,
                source.Chance,
                source.Cooldown.count(),
                source.Charges
            };
        }

        void AppendSpellProcJson(std::string& body, bool hasProc, EquipmentSpellProc const& proc)
        {
            if (!hasProc)
            {
                body += "null";
                return;
            }

            body += "{\"schoolMask\":" + std::to_string(proc.schoolMask) +
                ",\"spellFamilyName\":" + std::to_string(proc.spellFamilyName) +
                ",\"spellFamilyMask\":[" + std::to_string(proc.spellFamilyMask[0]) +
                "," + std::to_string(proc.spellFamilyMask[1]) +
                "," + std::to_string(proc.spellFamilyMask[2]) +
                "],\"procFlags\":" + std::to_string(proc.procFlags) +
                ",\"spellTypeMask\":" + std::to_string(proc.spellTypeMask) +
                ",\"spellPhaseMask\":" + std::to_string(proc.spellPhaseMask) +
                ",\"hitMask\":" + std::to_string(proc.hitMask) +
                ",\"attributesMask\":" + std::to_string(proc.attributesMask) +
                ",\"disableEffectsMask\":" + std::to_string(proc.disableEffectsMask) +
                ",\"procsPerMinute\":" + std::to_string(proc.procsPerMinute) +
                ",\"chance\":" + std::to_string(proc.chance) +
                ",\"cooldownMs\":" + std::to_string(proc.cooldownMs) +
                ",\"charges\":" + std::to_string(proc.charges) + "}";
        }

        struct EquipmentSpell
        {
            int32_t id = 0;
            uint8_t trigger = 0;
            int16_t charges = 0;
            float ppmRate = 0.0f;
            int32_t cooldownMs = 0;
            uint16_t category = 0;
            int32_t categoryCooldownMs = 0;
            std::string name;
            std::string nameRu;
            std::vector<EquipmentSpellEffect> effects;
            bool hasProc = false;
            EquipmentSpellProc proc;
        };

        struct EquipmentSetBonus
        {
            uint8_t requiredItems = 0;
            uint32_t spellId = 0;
            std::string spellName;
            std::string spellNameRu;
            std::vector<EquipmentSpellEffect> effects;
            bool hasProc = false;
            EquipmentSpellProc proc;
        };

        struct RandomPropertyEnchantment
        {
            uint32_t id = 0;
            uint32_t allocationPct = 0;
            std::string name;
            std::string nameRu;
            std::vector<EquipmentEnchantment::Effect> effects;
            bool hasProc = false;
            EquipmentEnchantmentProc proc;
        };

        RandomPropertyEnchantment BuildRandomPropertyEnchantment(uint32_t id, uint32_t allocationPct)
        {
            RandomPropertyEnchantment property;
            property.id = id;
            property.allocationPct = allocationPct;
            if (SpellItemEnchantmentEntry const* entry = sSpellItemEnchantmentStore.LookupEntry(id))
            {
                property.name = entry->description[LOCALE_enUS] ? entry->description[LOCALE_enUS] : "";
                property.nameRu = entry->description[LOCALE_ruRU] ? entry->description[LOCALE_ruRU] : property.name;
                for (std::size_t effectIndex = 0; effectIndex < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++effectIndex)
                {
                    uint32_t const type = entry->type[effectIndex];
                    uint32_t const amount = entry->amount[effectIndex];
                    uint32_t const spellId = entry->spellid[effectIndex];
                    if (type != 0 || amount != 0 || spellId != 0)
                        property.effects.push_back({type, amount, spellId});
                }
            }
            if (SpellEnchantProcEntry const* proc = sSpellMgr->GetSpellEnchantProcEvent(id))
            {
                property.hasProc = true;
                property.proc = BuildEnchantmentProc(*proc);
            }
            return property;
        }

        struct EquippedItem
        {
            uint8_t slot = 0;
            uint32_t itemEntry = 0;
            uint32_t instanceGuid = 0;
            int32_t randomPropertyId = 0;
            std::string randomPropertyName;
            std::string randomPropertyNameRu;
            std::vector<RandomPropertyEnchantment> randomPropertyEnchantments;
            uint32_t displayId = 0;
            std::string name;
            std::string nameRu;
            uint8_t quality = 0;
            uint16_t itemLevel = 0;
            uint8_t requiredLevel = 0;
            uint8_t inventoryType = 0;
            uint8_t itemClass = 0;
            uint8_t itemSubclass = 0;
            uint32_t flags = 0;
            int32_t maxCount = 0;
            uint8_t bonding = 0;
            uint32_t armor = 0;
            uint32_t block = 0;
            uint32_t delayMs = 0;
            uint32_t durability = 0;
            uint32_t maxDurability = 0;
            float rangedModRange = 0.0f;
            uint32_t sellPrice = 0;
            uint32_t allowableClass = 0;
            uint32_t allowableRace = 0;
            uint32_t requiredSkill = 0;
            uint32_t requiredSkillRank = 0;
            uint32_t requiredSpell = 0;
            uint32_t requiredHonorRank = 0;
            uint32_t requiredCityRank = 0;
            uint32_t requiredReputationFaction = 0;
            uint32_t requiredReputationRank = 0;
            uint32_t itemSetId = 0;
            std::string itemSetName;
            std::string itemSetNameRu;
            std::vector<EquipmentSetBonus> itemSetBonuses;
            int16_t itemLimitCategory = 0;
            std::string itemLimitCategoryName;
            uint32_t socketBonus = 0;
            std::string socketBonusName;
            std::string socketBonusNameRu;
            std::vector<EquipmentEnchantment::Effect> socketBonusEffects;
            bool hasSocketBonusProc = false;
            EquipmentEnchantmentProc socketBonusProc;
            uint32_t gemProperties = 0;
            std::array<EquipmentSocket, 3> sockets{};
            std::vector<EquipmentSpell> spells;
            std::vector<EquipmentEnchantment> enchantments;
            std::vector<EquipmentStat> stats;
            std::vector<EquipmentDamage> damage;
            std::array<int32_t, 6> resistances{};
            bool soulbound = false;
            bool unknownTemplate = true;
        };

        struct ProfessionSkill
        {
            uint16_t skill = 0;
            uint16_t value = 0;
            uint16_t max = 0;
        };

        ApiResponse MakeError(uint16_t status, std::string_view reason, std::string_view code)
        {
            return {status, std::string(reason),
                "{\"error\":{\"code\":\"" + EscapeJson(code) + "\"}}"};
        }

        bool DecodeUrlComponent(std::string_view encoded, std::string& decoded)
        {
            auto hexValue = [](char character) -> int
            {
                if (character >= '0' && character <= '9')
                    return character - '0';
                if (character >= 'a' && character <= 'f')
                    return character - 'a' + 10;
                if (character >= 'A' && character <= 'F')
                    return character - 'A' + 10;
                return -1;
            };

            decoded.clear();
            decoded.reserve(encoded.size());
            for (std::size_t index = 0; index < encoded.size(); ++index)
            {
                if (encoded[index] == '+')
                    decoded.push_back(' ');
                else if (encoded[index] == '%')
                {
                    if (index + 2 >= encoded.size())
                        return false;
                    int const high = hexValue(encoded[index + 1]);
                    int const low = hexValue(encoded[index + 2]);
                    if (high < 0 || low < 0)
                        return false;
                    decoded.push_back(static_cast<char>((high << 4) | low));
                    index += 2;
                }
                else
                    decoded.push_back(encoded[index]);
            }
            return true;
        }

        std::string Lowercase(std::string value)
        {
            std::transform(value.begin(), value.end(), value.begin(), [](unsigned char character)
            {
                return static_cast<char>(std::tolower(character));
            });
            return value;
        }

        bool ParseBoolean(std::string const& value, bool& output)
        {
            std::string const normalized = Lowercase(value);
            if (normalized == "true" || normalized == "1")
            {
                output = true;
                return true;
            }
            if (normalized == "false" || normalized == "0")
            {
                output = false;
                return true;
            }
            return false;
        }

        std::string TalentSpecName(uint8_t playerClass, uint32_t tree)
        {
            static constexpr std::array<std::array<std::string_view, 3>, 12> Specs = {{
                {"", "", ""},
                {"Arms", "Fury", "Protection"},
                {"Holy", "Protection", "Retribution"},
                {"Beast Mastery", "Marksmanship", "Survival"},
                {"Assassination", "Combat", "Subtlety"},
                {"Discipline", "Holy", "Shadow"},
                {"Blood", "Frost", "Unholy"},
                {"Elemental", "Enhancement", "Restoration"},
                {"Arcane", "Fire", "Frost"},
                {"Affliction", "Demonology", "Destruction"},
                {"", "", ""},
                {"Balance", "Feral Combat", "Restoration"}
            }};
            if (playerClass == 0 || playerClass >= Specs.size() || tree >= 3)
                return {};
            return std::string(Specs[playerClass][tree]);
        }

        std::vector<std::string_view> SuggestedRoles(uint8_t playerClass, int8_t tree)
        {
            std::vector<std::string_view> roles;
            switch (playerClass)
            {
                case 5:
                    if (tree == 0 || tree == 1)
                        roles.push_back("healer");
                    else if (tree == 2)
                        roles.push_back("dps");
                    break;
                case 7:
                    if (tree == 2)
                        roles.push_back("healer");
                    else if (tree == 0 || tree == 1)
                        roles.push_back("dps");
                    break;
                case 1:
                    if (tree == 2)
                        roles.push_back("tank");
                    else if (tree == 0 || tree == 1)
                        roles.push_back("dps");
                    break;
                case 2:
                    if (tree == 0)
                        roles.push_back("healer");
                    else if (tree == 1)
                        roles.push_back("tank");
                    else if (tree == 2)
                        roles.push_back("dps");
                    break;
                case 11:
                    if (tree == 0)
                        roles.push_back("dps");
                    else if (tree == 1)
                    {
                        roles.push_back("tank");
                        roles.push_back("dps");
                    }
                    else if (tree == 2)
                        roles.push_back("healer");
                    break;
                case 6:
                    if (tree == 0)
                        roles.push_back("tank");
                    else if (tree == 1 || tree == 2)
                        roles.push_back("dps");
                    break;
                case 3:
                case 4:
                case 8:
                case 9:
                    roles.push_back("dps");
                    break;
                default:
                    break;
            }
            return roles;
        }

        struct TalentSpellInfo
        {
            uint32_t tree = 0;
            uint32_t rank = 0;
            uint32_t classMask = 0;
        };

        std::unordered_map<uint32_t, TalentSpellInfo> const& TalentSpellLookup()
        {
            static std::unordered_map<uint32_t, TalentSpellInfo> const talents = []
            {
                std::unordered_map<uint32_t, TalentSpellInfo> result;
                for (TalentEntry const* talent : sTalentStore)
                {
                    TalentTabEntry const* tab = sTalentTabStore.LookupEntry(talent->TalentTab);
                    if (!tab)
                        continue;
                    for (std::size_t rank = 0; rank < talent->RankID.size(); ++rank)
                    {
                        if (talent->RankID[rank] != 0)
                            result.emplace(talent->RankID[rank], TalentSpellInfo{
                                tab->tabpage, static_cast<uint32_t>(rank + 1), tab->ClassMask});
                    }
                }
                return result;
            }();
            return talents;
        }

        template <typename T>
        bool ParseOptionalNumber(std::unordered_map<std::string, std::string> const& values,
            std::string_view key, std::optional<T>& output, T minimum = 0,
            T maximum = std::numeric_limits<T>::max())
        {
            auto const value = values.find(std::string(key));
            if (value == values.end())
                return true;

            T parsed = 0;
            auto const [end, error] = std::from_chars(value->second.data(),
                value->second.data() + value->second.size(), parsed);
            if (error != std::errc{} || end != value->second.data() + value->second.size() ||
                parsed < minimum || parsed > maximum)
                return false;

            output = parsed;
            return true;
        }

        bool ParseFilters(std::string_view target, CharacterFilters& filters)
        {
            std::unordered_map<std::string, std::string> values;
            std::size_t const queryStart = target.find('?');
            if (queryStart != std::string_view::npos)
            {
                std::string_view const query = target.substr(queryStart + 1);
                for (std::size_t start = 0; start < query.size();)
                {
                    std::size_t end = query.find('&', start);
                    if (end == std::string_view::npos)
                        end = query.size();
                    std::string_view const pair = query.substr(start, end - start);
                    std::size_t const equals = pair.find('=');
                    std::string key;
                    std::string value;
                    if (!DecodeUrlComponent(pair.substr(0, equals), key) ||
                        (equals != std::string_view::npos &&
                            !DecodeUrlComponent(pair.substr(equals + 1), value)) ||
                        key.empty() || !values.emplace(std::move(key), std::move(value)).second)
                        return false;
                    start = end + 1;
                }
            }

            static constexpr std::array<std::string_view, 18> AllowedKeys = {
                "limit", "cursor", "name", "accountId", "online", "bot", "botType", "race",
                "class", "minLevel", "maxLevel", "guildId", "mapId", "zoneId", "includeDeleted",
                "includeTalents", "guids", "names"
            };
            for (auto const& [key, value] : values)
            {
                (void)value;
                if (std::find(AllowedKeys.begin(), AllowedKeys.end(), key) == AllowedKeys.end())
                    return false;
            }

            std::optional<uint32_t> parsedLimit;
            if (!ParseOptionalNumber(values, "limit", parsedLimit, 1u, 1000u))
                return false;
            if (parsedLimit)
                filters.limit = *parsedLimit;

            std::optional<uint32_t> parsedCursor;
            if (!ParseOptionalNumber(values, "cursor", parsedCursor))
                return false;
            if (parsedCursor)
                filters.cursor = *parsedCursor;

            auto const name = values.find("name");
            if (name != values.end())
            {
                if (name->second.size() > 64 || std::any_of(name->second.begin(), name->second.end(),
                    [](unsigned char character) { return character < 0x20 || character == 0x7f; }))
                    return false;
                filters.namePrefix = name->second;
            }

            if (!ParseOptionalNumber(values, "accountId", filters.accountId, 1u) ||
                !ParseOptionalNumber(values, "guildId", filters.guildId, 1u) ||
                !ParseOptionalNumber(values, "mapId", filters.mapId) ||
                !ParseOptionalNumber(values, "zoneId", filters.zoneId))
                return false;

            auto const guids = values.find("guids");
            if (guids != values.end())
            {
                if (guids->second.empty())
                    return false;
                std::size_t start = 0;
                while (start < guids->second.size())
                {
                    std::size_t end = guids->second.find(',', start);
                    if (end == std::string::npos)
                        end = guids->second.size();
                    if (end == start ||
                        (end + 1 == guids->second.size() && guids->second[end] == ','))
                        return false;
                    uint32_t guid = 0;
                    auto const [parsedEnd, error] = std::from_chars(
                        guids->second.data() + start, guids->second.data() + end, guid);
                    if (error != std::errc{} || parsedEnd != guids->second.data() + end ||
                        guid == 0 || filters.guids.size() >= 100)
                        return false;
                    if (std::find(filters.guids.begin(), filters.guids.end(), guid) !=
                        filters.guids.end())
                        return false;
                    filters.guids.push_back(guid);
                    start = end + 1;
                }
            }

            auto const names = values.find("names");
            if (names != values.end())
            {
                if (names->second.empty())
                    return false;
                std::size_t start = 0;
                while (start < names->second.size())
                {
                    std::size_t end = names->second.find(',', start);
                    if (end == std::string::npos)
                        end = names->second.size();
                    std::string name = names->second.substr(start, end - start);
                    if (name.empty() || name.size() > 96 || filters.exactNames.size() >= 40 ||
                        std::any_of(name.begin(), name.end(), [](unsigned char character)
                            { return character < 0x20 || character == 0x7f; }))
                        return false;
                    if (std::find(filters.exactNames.begin(), filters.exactNames.end(), name) !=
                        filters.exactNames.end())
                        return false;
                    filters.exactNames.push_back(std::move(name));
                    if (end + 1 == names->second.size() && names->second[end] == ',')
                        return false;
                    start = end + 1;
                }
            }

            std::optional<uint32_t> race;
            std::optional<uint32_t> playerClass;
            std::optional<uint32_t> minLevel;
            std::optional<uint32_t> maxLevel;
            if (!ParseOptionalNumber(values, "race", race, 1u, 255u) ||
                !ParseOptionalNumber(values, "class", playerClass, 1u, 255u) ||
                !ParseOptionalNumber(values, "minLevel", minLevel, 1u, 255u) ||
                !ParseOptionalNumber(values, "maxLevel", maxLevel, 1u, 255u))
                return false;
            if (race)
                filters.race = static_cast<uint8_t>(*race);
            if (playerClass)
                filters.playerClass = static_cast<uint8_t>(*playerClass);
            if (minLevel)
                filters.minLevel = static_cast<uint8_t>(*minLevel);
            if (maxLevel)
                filters.maxLevel = static_cast<uint8_t>(*maxLevel);
            if (filters.minLevel && filters.maxLevel && *filters.minLevel > *filters.maxLevel)
                return false;

            for (auto [key, output] : {std::pair<std::string_view, std::optional<bool>*>{"online", &filters.online},
                {"bot", &filters.bot}, {"includeDeleted", nullptr}, {"includeTalents", nullptr}})
            {
                auto const value = values.find(std::string(key));
                if (value == values.end())
                    continue;
                bool parsed = false;
                if (!ParseBoolean(value->second, parsed))
                    return false;
                if (output)
                    *output = parsed;
                else if (key == "includeDeleted")
                    filters.includeDeleted = parsed;
                else
                    filters.includeTalents = parsed;
            }

            auto const botType = values.find("botType");
            if (botType != values.end())
            {
                std::string const normalized = Lowercase(botType->second);
                if (normalized != "player" && normalized != "random" && normalized != "addclass" &&
                    normalized != "unknown")
                    return false;
                filters.botType = normalized;
            }

            return true;
        }

        std::string SqlString(std::string_view value)
        {
            std::string escaped;
            escaped.reserve(value.size() + 2);
            escaped.push_back('\'');
            for (char character : value)
            {
                if (character == '\\' || character == '\'')
                    escaped.push_back('\\');
                escaped.push_back(character);
            }
            escaped.push_back('\'');
            return escaped;
        }

        std::string NamePattern(std::string_view prefix)
        {
            std::string pattern;
            pattern.reserve(prefix.size() + 1);
            for (char character : prefix)
            {
                if (character == '=' || character == '%' || character == '_')
                    pattern.push_back('=');
                pattern.push_back(character);
            }
            pattern.push_back('%');
            return pattern;
        }

        CharacterRecord ReadCharacter(Field* fields, bool includeTalents = false)
        {
            CharacterRecord character;
            character.guid = fields[0].Get<uint32>();
            character.accountId = fields[1].Get<uint32>();
            character.name = fields[2].Get<std::string>();
            character.race = fields[3].Get<uint8>();
            character.playerClass = fields[4].Get<uint8>();
            character.gender = fields[5].Get<uint8>();
            character.level = fields[6].Get<uint8>();
            character.xp = fields[7].Get<uint32>();
            character.money = fields[8].Get<uint32>();
            character.guildId = fields[9].Get<uint32>();
            character.guildName = fields[10].Get<std::string>();
            character.mapId = fields[11].Get<uint16>();
            character.zoneId = fields[12].Get<uint16>();
            character.deleted = !fields[13].IsNull();
            character.x = fields[14].Get<float>();
            character.y = fields[15].Get<float>();
            character.z = fields[16].Get<float>();
            character.orientation = fields[17].Get<float>();
            character.activeTalentGroup = fields[18].Get<uint8>();
            character.talentGroupsCount = fields[19].Get<uint8>();
            character.available = fields[20].Get<bool>();
            if (includeTalents)
            {
                character.hasTalentSpells = true;
                std::string const spells = fields[21].Get<std::string>();
                for (std::size_t start = 0; start < spells.size();)
                {
                    std::size_t end = spells.find(',', start);
                    if (end == std::string::npos)
                        end = spells.size();
                    uint32_t spell = 0;
                    auto const [parsedEnd, error] = std::from_chars(
                        spells.data() + start, spells.data() + end, spell);
                    if (error == std::errc{} && parsedEnd == spells.data() + end && spell != 0)
                        character.talentSpells.push_back(spell);
                    start = end + 1;
                }

                std::array<uint32_t, 3> pointsByTree{};
                auto const& talentLookup = TalentSpellLookup();
                for (uint32_t spell : character.talentSpells)
                {
                    auto const talent = talentLookup.find(spell);
                    if (talent == talentLookup.end())
                        continue;

                    if (character.playerClass == 0 || character.playerClass > 32 ||
                        (talent->second.classMask & (1u << (character.playerClass - 1))) == 0)
                        continue;
                    if (talent->second.tree < character.talentPoints.size())
                        pointsByTree[talent->second.tree] += talent->second.rank;
                }

                character.hasTalentSummary = true;
                character.talentPoints = pointsByTree;
                uint32_t bestPoints = 0;
                int8_t bestTree = -1;
                for (std::size_t tree = 0; tree < pointsByTree.size(); ++tree)
                {
                    if (pointsByTree[tree] > bestPoints)
                    {
                        bestPoints = pointsByTree[tree];
                        bestTree = static_cast<int8_t>(tree);
                    }
                }
                if (bestTree >= 0)
                    character.spec = TalentSpecName(character.playerClass, bestTree);
                character.suggestedRoles = SuggestedRoles(character.playerClass, bestTree);
            }
            return character;
        }

        std::string CharacterSelect(std::string_view where, bool includeTalents = false)
        {
            std::string sql = "SELECT c.guid, COALESCE(NULLIF(c.account, 0), c.deleteInfos_Account, 0), "
                "COALESCE(NULLIF(c.name COLLATE utf8mb4_unicode_ci, ''), "
                "c.deleteInfos_Name COLLATE utf8mb4_unicode_ci, ''), c.race, c.class, c.gender, c.level, "
                "c.xp, c.money, COALESCE(gm.guildid, 0), COALESCE(g.name, ''), c.map, c.zone, c.deleteDate, "
                "c.position_x, c.position_y, c.position_z, c.orientation, "
                "c.activeTalentGroup, c.talentGroupsCount, "
                "NOT EXISTS (SELECT 1 FROM group_member gm2 WHERE gm2.memberGuid = c.guid) "
                "FROM characters c "
                "LEFT JOIN guild_member gm ON gm.guid = c.guid "
                "LEFT JOIN guild g ON g.guildid = gm.guildid WHERE " + std::string(where);
            if (includeTalents)
            {
                std::size_t const from = sql.find(" FROM characters c ");
                sql.insert(from,
                    ",(SELECT COALESCE(GROUP_CONCAT(DISTINCT ct.spell ORDER BY ct.spell SEPARATOR ','), '') "
                    "FROM character_talent ct WHERE ct.guid = c.guid "
                    "AND (ct.specMask & (1 << c.activeTalentGroup)) != 0) ");
            }
            return sql;
        }

        std::string BuildListSql(CharacterFilters const& filters, uint32_t cursor,
            uint32_t batchSize, std::vector<uint32_t> const* onlineGuids)
        {
            std::string where = "c.guid > " + std::to_string(cursor);
            if (!filters.includeDeleted)
                where += " AND c.deleteDate IS NULL";
            if (filters.accountId)
                where += " AND COALESCE(NULLIF(c.account, 0), c.deleteInfos_Account, 0) = " +
                    std::to_string(*filters.accountId);
            if (!filters.guids.empty())
            {
                where += " AND c.guid IN (";
                for (std::size_t index = 0; index < filters.guids.size(); ++index)
                {
                    if (index != 0)
                        where += ',';
                    where += std::to_string(filters.guids[index]);
                }
                where += ')';
            }
            if (!filters.namePrefix.empty())
                where += " AND COALESCE(NULLIF(c.name COLLATE utf8mb4_unicode_ci, ''), "
                    "c.deleteInfos_Name COLLATE utf8mb4_unicode_ci, '') LIKE " +
                    SqlString(NamePattern(filters.namePrefix)) +
                    " COLLATE utf8mb4_unicode_ci ESCAPE '='";
            if (!filters.exactNames.empty())
            {
                where += " AND c.name IN (";
                for (std::size_t index = 0; index < filters.exactNames.size(); ++index)
                {
                    if (index != 0)
                        where += ',';
                    where += SqlString(filters.exactNames[index]);
                }
                where += ')';
            }
            if (filters.race)
                where += " AND c.race = " + std::to_string(*filters.race);
            if (filters.playerClass)
                where += " AND c.class = " + std::to_string(*filters.playerClass);
            if (filters.minLevel)
                where += " AND c.level >= " + std::to_string(*filters.minLevel);
            if (filters.maxLevel)
                where += " AND c.level <= " + std::to_string(*filters.maxLevel);
            if (filters.guildId)
                where += " AND gm.guildid = " + std::to_string(*filters.guildId);

            if (onlineGuids)
            {
                if (onlineGuids->empty())
                    where += " AND 1 = 0";
                else
                {
                    where += " AND c.guid IN (";
                    for (std::size_t index = 0; index < onlineGuids->size(); ++index)
                    {
                        if (index != 0)
                            where += ',';
                        where += std::to_string((*onlineGuids)[index]);
                    }
                    where += ')';
                }
            }

            return CharacterSelect(where, filters.includeTalents) + " ORDER BY c.guid ASC LIMIT " + std::to_string(batchSize);
        }

        std::string BotTypeName(uint32_t accountId, bool available,
            std::unordered_map<uint32_t, uint8_t> const& accountTypes)
        {
            if (!available)
                return "unknown";
            auto const account = accountTypes.find(accountId);
            if (account == accountTypes.end() || account->second == 0)
                return "player";
            if (account->second == 1)
                return "random";
            if (account->second == 2)
                return "addclass";
            return "unknown";
        }

        bool IsBotType(std::string_view type)
        {
            return type == "random" || type == "addclass";
        }

        std::string BuildCharacterJson(CharacterRecord const& character, bool online, bool bot,
            std::string_view botType, uint32_t mapId, uint32_t zoneId,
            std::optional<PlayerSnapshot> const& runtime)
        {
            auto finite = [](float value) { return std::isfinite(value) ? value : 0.0f; };
            float const x = finite(runtime ? runtime->x : character.x);
            float const y = finite(runtime ? runtime->y : character.y);
            float const z = finite(runtime ? runtime->z : character.z);
            float const orientation = finite(runtime ? runtime->orientation : character.orientation);
            std::string body = "{\"guid\":" + std::to_string(character.guid) +
                ",\"accountId\":" + std::to_string(character.accountId) +
                ",\"name\":\"" + EscapeJson(character.name) + "\"" +
                ",\"race\":" + std::to_string(character.race) +
                ",\"class\":" + std::to_string(character.playerClass) +
                ",\"gender\":" + std::to_string(character.gender) +
                ",\"level\":" + std::to_string(character.level) +
                ",\"guildId\":" + std::to_string(character.guildId) +
                ",\"guildName\":\"" + EscapeJson(character.guildName) + "\"" +
                ",\"online\":" + (online ? "true" : "false") +
                ",\"available\":" + ((!online || character.available) ? "true" : "false") +
                ",\"bot\":" + (bot ? "true" : "false") +
                ",\"botType\":\"" + EscapeJson(botType) + "\"" +
                ",\"deleted\":" + (character.deleted ? "true" : "false") +
                ",\"mapId\":" + std::to_string(mapId) +
                ",\"zoneId\":" + std::to_string(zoneId) +
                ",\"location\":{\"mapId\":" + std::to_string(mapId) +
                ",\"zoneId\":" + std::to_string(zoneId) +
                ",\"x\":" + std::to_string(x) + ",\"y\":" + std::to_string(y) +
                ",\"z\":" + std::to_string(z) + ",\"orientation\":" +
                std::to_string(orientation) + "}}";
            if (character.hasTalentSpells)
            {
                std::string talentJson = ",\"talentSpells\":[";
                for (std::size_t index = 0; index < character.talentSpells.size(); ++index)
                {
                    if (index != 0)
                        talentJson += ',';
                    talentJson += std::to_string(character.talentSpells[index]);
                }
                talentJson += ']';
                body.insert(body.size() - 1, talentJson);
            }
            if (character.hasTalentSummary)
            {
                std::string talentSummary = ",\"spec\":\"" + EscapeJson(character.spec) +
                    "\",\"talentPoints\":[" + std::to_string(character.talentPoints[0]) + "," +
                    std::to_string(character.talentPoints[1]) + "," +
                    std::to_string(character.talentPoints[2]) + "],\"suggestedRoles\":[";
                for (std::size_t index = 0; index < character.suggestedRoles.size(); ++index)
                {
                    if (index != 0)
                        talentSummary += ',';
                    talentSummary += "\"" + std::string(character.suggestedRoles[index]) + "\"";
                }
                talentSummary += ']';
                body.insert(body.size() - 1, talentSummary);
            }
            return body;
        }

        std::string_view ProfessionName(uint16_t skill)
        {
            switch (skill)
            {
                case 171: return "Alchemy";
                case 164: return "Blacksmithing";
                case 333: return "Enchanting";
                case 202: return "Engineering";
                case 182: return "Herbalism";
                case 773: return "Inscription";
                case 755: return "Jewelcrafting";
                case 165: return "Leatherworking";
                case 186: return "Mining";
                case 393: return "Skinning";
                case 197: return "Tailoring";
                case 185: return "Cooking";
                case 129: return "First Aid";
                case 356: return "Fishing";
                default: return "Unknown";
            }
        }

        std::string BuildCharacterStatsJson(CharacterStatSnapshot const& stats, bool hasCurrentValues)
        {
            static constexpr std::array<std::string_view, 7> powerTypes = {
                "mana", "rage", "focus", "energy", "happiness", "rune", "runicPower"
            };
            auto currentValue = [hasCurrentValues](uint32_t value)
            {
                return hasCurrentValues ? std::to_string(value) : std::string("null");
            };
            std::string body = "{\"source\":\"" +
                std::string(hasCurrentValues ? "live" : "saved") + "\",\"primaryAttributes\":{" +
                "\"strength\":" + std::to_string(stats.primaryAttributes[0]) +
                ",\"agility\":" + std::to_string(stats.primaryAttributes[1]) +
                ",\"stamina\":" + std::to_string(stats.primaryAttributes[2]) +
                ",\"intellect\":" + std::to_string(stats.primaryAttributes[3]) +
                ",\"spirit\":" + std::to_string(stats.primaryAttributes[4]) +
                "},\"health\":{\"current\":" + currentValue(stats.health) +
                ",\"maximum\":" + std::to_string(stats.maxHealth) + "},\"powers\":[";
            for (std::size_t index = 0; index < powerTypes.size(); ++index)
            {
                if (index != 0)
                    body += ',';
                body += "{\"type\":\"" + std::string(powerTypes[index]) +
                    "\",\"current\":" + currentValue(stats.powers[index]) +
                    ",\"maximum\":" + std::to_string(stats.maxPowers[index]) + "}";
            }
            body += "],\"armor\":" + std::to_string(stats.armor) +
                ",\"resistances\":{\"holy\":" + std::to_string(stats.resistances[0]) +
                ",\"fire\":" + std::to_string(stats.resistances[1]) +
                ",\"nature\":" + std::to_string(stats.resistances[2]) +
                ",\"frost\":" + std::to_string(stats.resistances[3]) +
                ",\"shadow\":" + std::to_string(stats.resistances[4]) +
                ",\"arcane\":" + std::to_string(stats.resistances[5]) +
                "},\"combat\":{\"attackPower\":" + std::to_string(stats.attackPower) +
                ",\"rangedAttackPower\":" + std::to_string(stats.rangedAttackPower) +
                ",\"spellPower\":" + std::to_string(stats.spellPower) +
                ",\"resilience\":" + std::to_string(stats.resilience) +
                ",\"blockPct\":" + std::to_string(stats.blockPct) +
                ",\"dodgePct\":" + std::to_string(stats.dodgePct) +
                ",\"parryPct\":" + std::to_string(stats.parryPct) +
                ",\"critPct\":" + std::to_string(stats.critPct) +
                ",\"rangedCritPct\":" + std::to_string(stats.rangedCritPct) +
                ",\"spellCritPct\":" + std::to_string(stats.spellCritPct) + "}}";
            return body;
        }

        std::string BuildProfileJson(CharacterRecord const& character, bool online, bool bot,
            std::string_view botType, uint32_t mapId, uint32_t zoneId,
            std::optional<PlayerSnapshot> const& runtime, std::optional<CharacterStatSnapshot> const& stats,
            std::vector<EquippedItem> const& equipment, std::vector<ProfessionSkill> const& professions)
        {
            std::string body = BuildCharacterJson(character, online, bot, botType, mapId, zoneId, runtime);
            body.pop_back();
            body += ",\"xp\":" + std::to_string(character.xp) +
                ",\"money\":" + std::to_string(character.money) +
                ",\"talents\":{\"activeGroup\":" + std::to_string(character.activeTalentGroup) +
                ",\"totalGroups\":" + std::to_string(character.talentGroupsCount) +
                "},\"equipmentMetadataVersion\":2,\"equipment\":[";
            std::unordered_map<uint32_t, uint8_t> setCounts;
            for (EquippedItem const& item : equipment)
                if (item.itemSetId != 0)
                    ++setCounts[item.itemSetId];
            for (std::size_t index = 0; index < equipment.size(); ++index)
            {
                if (index != 0)
                    body += ',';
                EquippedItem const& item = equipment[index];
                body += "{\"slot\":" + std::to_string(item.slot) +
                    ",\"itemEntry\":" + std::to_string(item.itemEntry) +
                    ",\"instanceGuid\":" + std::to_string(item.instanceGuid) +
                    ",\"randomPropertyId\":" + std::to_string(item.randomPropertyId) +
                    ",\"randomPropertyName\":\"" + EscapeJson(item.randomPropertyName) + "\"" +
                    ",\"randomPropertyNameRu\":\"" + EscapeJson(item.randomPropertyNameRu) + "\"" +
                    ",\"displayId\":" + std::to_string(item.displayId) +
                    ",\"name\":\"" + EscapeJson(item.name) + "\"" +
                    ",\"nameRu\":\"" + EscapeJson(item.nameRu) + "\"" +
                    ",\"quality\":" + std::to_string(item.quality) +
                    ",\"itemLevel\":" + std::to_string(item.itemLevel) +
                    ",\"requiredLevel\":" + std::to_string(item.requiredLevel) +
                    ",\"inventoryType\":" + std::to_string(item.inventoryType) +
                    ",\"itemClass\":" + std::to_string(item.itemClass) +
                    ",\"itemSubclass\":" + std::to_string(item.itemSubclass) +
                    ",\"flags\":" + std::to_string(item.flags) +
                    ",\"maxCount\":" + std::to_string(item.maxCount) +
                    ",\"bonding\":" + std::to_string(item.bonding) +
                    ",\"armor\":" + std::to_string(item.armor) +
                    ",\"block\":" + std::to_string(item.block) +
                    ",\"delayMs\":" + std::to_string(item.delayMs) +
                    ",\"durability\":" + std::to_string(item.durability) +
                    ",\"maxDurability\":" + std::to_string(item.maxDurability) +
                    ",\"rangedModRange\":" + std::to_string(item.rangedModRange) +
                    ",\"sellPrice\":" + std::to_string(item.sellPrice) +
                    ",\"allowableClass\":" + std::to_string(item.allowableClass) +
                    ",\"allowableRace\":" + std::to_string(item.allowableRace) +
                    ",\"requiredSkill\":" + std::to_string(item.requiredSkill) +
                    ",\"requiredSkillRank\":" + std::to_string(item.requiredSkillRank) +
                    ",\"requiredSpell\":" + std::to_string(item.requiredSpell) +
                    ",\"requiredHonorRank\":" + std::to_string(item.requiredHonorRank) +
                    ",\"requiredCityRank\":" + std::to_string(item.requiredCityRank) +
                    ",\"requiredReputationFaction\":" + std::to_string(item.requiredReputationFaction) +
                    ",\"requiredReputationRank\":" + std::to_string(item.requiredReputationRank) +
                    ",\"itemSetId\":" + std::to_string(item.itemSetId) +
                    ",\"itemSetName\":\"" + EscapeJson(item.itemSetName) + "\"" +
                    ",\"itemSetNameRu\":\"" + EscapeJson(item.itemSetNameRu) + "\"" +
                    ",\"itemSetEquippedCount\":" + std::to_string(item.itemSetId == 0 ? 0 : setCounts[item.itemSetId]) +
                    ",\"itemSetBonuses\":[";
                for (std::size_t bonusIndex = 0; bonusIndex < item.itemSetBonuses.size(); ++bonusIndex)
                {
                    if (bonusIndex != 0)
                        body += ',';
                    EquipmentSetBonus const& bonus = item.itemSetBonuses[bonusIndex];
                    body += "{\"requiredItems\":" + std::to_string(bonus.requiredItems) +
                        ",\"spellId\":" + std::to_string(bonus.spellId) +
                        ",\"spellName\":\"" + EscapeJson(bonus.spellName) + "\"" +
                        ",\"spellNameRu\":\"" + EscapeJson(bonus.spellNameRu) + "\"" +
                        ",\"proc\":";
                    AppendSpellProcJson(body, bonus.hasProc, bonus.proc);
                    body += ",\"active\":";
                    body += (setCounts[item.itemSetId] >= bonus.requiredItems ? "true" : "false");
                    body += ",\"effects\":[";
                    for (std::size_t effectIndex = 0; effectIndex < bonus.effects.size(); ++effectIndex)
                    {
                        if (effectIndex != 0)
                            body += ',';
                        EquipmentSpellEffect const& effect = bonus.effects[effectIndex];
                        body += "{\"index\":" + std::to_string(effect.index) +
                            ",\"type\":" + std::to_string(effect.type) +
                            ",\"auraType\":" + std::to_string(effect.auraType) +
                            ",\"basePoints\":" + std::to_string(effect.basePoints) +
                            ",\"dieSides\":" + std::to_string(effect.dieSides) +
                            ",\"realPointsPerLevel\":" + std::to_string(effect.realPointsPerLevel) +
                            ",\"triggerSpell\":" + std::to_string(effect.triggerSpell) +
                            ",\"miscValue\":" + std::to_string(effect.miscValue) +
                            ",\"miscValueB\":" + std::to_string(effect.miscValueB) +
                            ",\"amplitudeMs\":" + std::to_string(effect.amplitudeMs) + "}";
                    }
                    body += "]}";
                }
                body += "],\"itemLimitCategory\":" + std::to_string(item.itemLimitCategory) +
                    ",\"itemLimitCategoryName\":\"" + EscapeJson(item.itemLimitCategoryName) + "\"" +
                    ",\"socketBonus\":" + std::to_string(item.socketBonus) +
                    ",\"socketBonusName\":\"" + EscapeJson(item.socketBonusName) + "\"" +
                    ",\"socketBonusNameRu\":\"" + EscapeJson(item.socketBonusNameRu) + "\"" +
                    ",\"socketBonusProc\":";
                AppendEnchantmentProcJson(body, item.hasSocketBonusProc, item.socketBonusProc);
                body += ",\"socketBonusEffects\":[";
                for (std::size_t effectIndex = 0; effectIndex < item.socketBonusEffects.size(); ++effectIndex)
                {
                    if (effectIndex != 0)
                        body += ',';
                    EquipmentEnchantment::Effect const& effect = item.socketBonusEffects[effectIndex];
                    body += "{\"type\":" + std::to_string(effect.type) +
                        ",\"amount\":" + std::to_string(effect.amount) +
                        ",\"spellId\":" + std::to_string(effect.spellId) + "}";
                }
                body += "],\"gemProperties\":" + std::to_string(item.gemProperties) +
                    ",\"sockets\":[";
                bool firstSocket = true;
                for (EquipmentSocket const& socket : item.sockets)
                {
                    if (socket.color == 0 && socket.content == 0)
                        continue;
                    if (!firstSocket)
                        body += ',';
                    firstSocket = false;
                    body += "{\"color\":" + std::to_string(socket.color) +
                        ",\"content\":" + std::to_string(socket.content) + "}";
                }
                body += "],\"spells\":[";
                for (std::size_t spellIndex = 0; spellIndex < item.spells.size(); ++spellIndex)
                {
                    if (spellIndex != 0)
                        body += ',';
                    EquipmentSpell const& spell = item.spells[spellIndex];
                    body += "{\"id\":" + std::to_string(spell.id) +
                        ",\"trigger\":" + std::to_string(spell.trigger) +
                        ",\"charges\":" + std::to_string(spell.charges) +
                        ",\"ppmRate\":" + std::to_string(spell.ppmRate) +
                        ",\"cooldownMs\":" + std::to_string(spell.cooldownMs) +
                        ",\"category\":" + std::to_string(spell.category) +
                        ",\"categoryCooldownMs\":" + std::to_string(spell.categoryCooldownMs) +
                        ",\"name\":\"" + EscapeJson(spell.name) + "\"" +
                        ",\"nameRu\":\"" + EscapeJson(spell.nameRu) + "\"" +
                        ",\"proc\":";
                    AppendSpellProcJson(body, spell.hasProc, spell.proc);
                    body += ",\"effects\":[";
                    for (std::size_t effectIndex = 0; effectIndex < spell.effects.size(); ++effectIndex)
                    {
                        if (effectIndex != 0)
                            body += ',';
                        EquipmentSpellEffect const& effect = spell.effects[effectIndex];
                        body += "{\"index\":" + std::to_string(effect.index) +
                            ",\"type\":" + std::to_string(effect.type) +
                            ",\"auraType\":" + std::to_string(effect.auraType) +
                            ",\"basePoints\":" + std::to_string(effect.basePoints) +
                            ",\"dieSides\":" + std::to_string(effect.dieSides) +
                            ",\"realPointsPerLevel\":" + std::to_string(effect.realPointsPerLevel) +
                            ",\"triggerSpell\":" + std::to_string(effect.triggerSpell) +
                            ",\"miscValue\":" + std::to_string(effect.miscValue) +
                            ",\"miscValueB\":" + std::to_string(effect.miscValueB) +
                            ",\"amplitudeMs\":" + std::to_string(effect.amplitudeMs) + "}";
                    }
                    body += "]}";
                }
                body += "],\"randomPropertyEnchantments\":[";
                for (std::size_t propertyIndex = 0; propertyIndex < item.randomPropertyEnchantments.size(); ++propertyIndex)
                {
                    if (propertyIndex != 0)
                        body += ',';
                    RandomPropertyEnchantment const& property = item.randomPropertyEnchantments[propertyIndex];
                    body += "{\"id\":" + std::to_string(property.id) +
                        ",\"allocationPct\":" + std::to_string(property.allocationPct) +
                        ",\"name\":\"" + EscapeJson(property.name) + "\"" +
                        ",\"nameRu\":\"" + EscapeJson(property.nameRu) + "\"" +
                        ",\"proc\":";
                    AppendEnchantmentProcJson(body, property.hasProc, property.proc);
                    body += ",\"effects\":[";
                    for (std::size_t effectIndex = 0; effectIndex < property.effects.size(); ++effectIndex)
                    {
                        if (effectIndex != 0)
                            body += ',';
                        EquipmentEnchantment::Effect const& effect = property.effects[effectIndex];
                        body += "{\"type\":" + std::to_string(effect.type) +
                            ",\"amount\":" + std::to_string(effect.amount) +
                            ",\"spellId\":" + std::to_string(effect.spellId) + "}";
                    }
                    body += "]}";
                }
                body += "],\"enchantments\":[";
                for (std::size_t enchantmentIndex = 0; enchantmentIndex < item.enchantments.size(); ++enchantmentIndex)
                {
                    if (enchantmentIndex != 0)
                        body += ',';
                    EquipmentEnchantment const& enchantment = item.enchantments[enchantmentIndex];
                    body += "{\"slot\":" + std::to_string(enchantment.slot) +
                        ",\"id\":" + std::to_string(enchantment.id) +
                        ",\"durationMs\":" + std::to_string(enchantment.durationMs) +
                        ",\"charges\":" + std::to_string(enchantment.charges) +
                        ",\"name\":\"" + EscapeJson(enchantment.name) + "\"" +
                        ",\"nameRu\":\"" + EscapeJson(enchantment.nameRu) + "\"" +
                        ",\"gemId\":" + std::to_string(enchantment.gemId) +
                        ",\"proc\":";
                    AppendEnchantmentProcJson(body, enchantment.hasProc, enchantment.proc);
                    body += ",\"effects\":[";
                    for (std::size_t effectIndex = 0; effectIndex < enchantment.effects.size(); ++effectIndex)
                    {
                        if (effectIndex != 0)
                            body += ',';
                        EquipmentEnchantment::Effect const& effect = enchantment.effects[effectIndex];
                        body += "{\"type\":" + std::to_string(effect.type) +
                            ",\"amount\":" + std::to_string(effect.amount) +
                            ",\"spellId\":" + std::to_string(effect.spellId) + "}";
                    }
                    body += "]}";
                }
                body += "],\"stats\":[";
                for (std::size_t statIndex = 0; statIndex < item.stats.size(); ++statIndex)
                {
                    if (statIndex != 0)
                        body += ',';
                    EquipmentStat const& stat = item.stats[statIndex];
                    body += "{\"type\":" + std::to_string(stat.type) +
                        ",\"value\":" + std::to_string(stat.value) + "}";
                }
                body += "],\"damage\":[";
                for (std::size_t damageIndex = 0; damageIndex < item.damage.size(); ++damageIndex)
                {
                    if (damageIndex != 0)
                        body += ',';
                    EquipmentDamage const& damage = item.damage[damageIndex];
                    body += "{\"min\":" + std::to_string(damage.min) +
                        ",\"max\":" + std::to_string(damage.max) +
                        ",\"type\":" + std::to_string(damage.type) + "}";
                }
                body += std::string("],\"resistances\":{") +
                    "\"holy\":" + std::to_string(item.resistances[0]) +
                    ",\"fire\":" + std::to_string(item.resistances[1]) +
                    ",\"nature\":" + std::to_string(item.resistances[2]) +
                    ",\"frost\":" + std::to_string(item.resistances[3]) +
                    ",\"shadow\":" + std::to_string(item.resistances[4]) +
                    ",\"arcane\":" + std::to_string(item.resistances[5]) +
                    "},\"soulbound\":" + (item.soulbound ? "true" : "false") +
                    ",\"unknownTemplate\":" + (item.unknownTemplate ? "true" : "false") + "}";
            }
            body += "],\"stats\":";
            body += stats ? BuildCharacterStatsJson(*stats, online) : "null";
            body += ",\"professions\":[";
            for (std::size_t index = 0; index < professions.size(); ++index)
            {
                if (index != 0)
                    body += ',';
                ProfessionSkill const& skill = professions[index];
                body += "{\"skillId\":" + std::to_string(skill.skill) +
                    ",\"name\":\"" + std::string(ProfessionName(skill.skill)) + "\"" +
                    ",\"value\":" + std::to_string(skill.value) +
                    ",\"max\":" + std::to_string(skill.max) + "}";
            }
            body += "]}";
            return body;
        }

        std::string BuildListResponse(std::vector<std::string> const& data, bool hasMore,
            std::optional<uint32_t> cursor)
        {
            std::string body = "{\"data\":[";
            for (std::size_t index = 0; index < data.size(); ++index)
            {
                if (index != 0)
                    body += ',';
                body += data[index];
            }
            body += "],\"hasMore\":";
            body += hasMore ? "true" : "false";
            body += ",\"nextCursor\":";
            body += cursor ? std::to_string(*cursor) : "null";
            body += '}';
            return body;
        }
    }

    struct CharactersApi::BotTypeSnapshot
    {
        bool available = false;
        std::unordered_map<uint32_t, uint8_t> accountTypes;
    };

    struct CharactersApi::ListState
    {
        CharacterFilters filters;
        Completion completion;
        std::unordered_map<uint32_t, PlayerSnapshot> onlineByGuid;
        std::shared_ptr<BotTypeSnapshot const> botTypes;
        std::vector<uint32_t> onlineCandidates;
        std::size_t onlineCandidateOffset = 0;
        uint32_t scanPosition = 0;
        uint32_t rowsScanned = 0;
        std::vector<CharacterRecord> records;
        std::vector<std::string> serializedRecords;
        bool finished = false;
    };

    struct CharactersApi::ProfileState
    {
        uint32_t guid = 0;
        Completion completion;
        CharacterRecord character;
        std::optional<PlayerSnapshot> runtime;
        std::shared_ptr<BotTypeSnapshot const> botTypes;
        std::vector<EquippedItem> equipment;
        std::vector<ProfessionSkill> professions;
        std::optional<CharacterStatSnapshot> stats;
        bool finished = false;
    };

    struct CharactersApi::ItemLevelState
    {
        Completion completion;
        std::vector<uint32_t> guids;
        std::unordered_map<uint32_t, std::string> namesByGuid;
        std::unordered_map<uint32_t, std::vector<uint32_t>> itemEntriesByGuid;
        std::unordered_map<uint32_t, uint16_t> itemLevelsByEntry;
        bool finished = false;
    };

    CharactersApi::CharactersApi(boost::asio::strand<boost::asio::io_context::executor_type> strand)
        : _strand(std::move(strand)), _callbackTimer(_strand),
          _botTypes(std::make_shared<BotTypeSnapshot>()) { }

    CharactersApi::~CharactersApi()
    {
        Stop();
    }

    void CharactersApi::Start()
    {
        if (_started)
            return;
        _started = true;
        _stopped = false;
        ScheduleCallbackPoll();
        StartBotTypeWorker();
    }

    void CharactersApi::Stop()
    {
        if (_stopped)
            return;
        _stopped = true;
        _callbackTimer.cancel();
        StopBotTypeWorker();
    }

    bool CharactersApi::ReserveRequest()
    {
        if (!_started || _stopped || _activeRequests >= MaxConcurrentRequests)
            return false;
        ++_activeRequests;
        return true;
    }

    void CharactersApi::CompleteRequest(Completion completion, ApiResponse response)
    {
        if (_activeRequests > 0)
            --_activeRequests;
        if (completion)
            completion(std::move(response));
    }

    bool CharactersApi::SubmitQuery(std::string sql, std::function<void(QueryResult)> callback)
    {
        if (_stopped || _activeQueries >= MaxConcurrentRequests)
            return false;

        try
        {
            ++_activeQueries;
            QueryCallback query = CharacterDatabase.AsyncQuery(sql).WithCallback(
                [this, callback = std::move(callback)](QueryResult result) mutable
            {
                if (_activeQueries > 0)
                    --_activeQueries;
                callback(std::move(result));
            });
            _queryCallbacks.AddCallback(std::move(query));
            return true;
        }
        catch (std::exception const& exception)
        {
            if (_activeQueries > 0)
                --_activeQueries;
            LOG_ERROR("server-api.characters", "Could not enqueue character database query: {}", exception.what());
            return false;
        }
    }

    bool CharactersApi::SubmitWorldQuery(std::string sql, std::function<void(QueryResult)> callback)
    {
        if (_stopped || _activeQueries >= MaxConcurrentRequests)
            return false;

        try
        {
            ++_activeQueries;
            QueryCallback query = WorldDatabase.AsyncQuery(sql).WithCallback(
                [this, callback = std::move(callback)](QueryResult result) mutable
            {
                if (_activeQueries > 0)
                    --_activeQueries;
                callback(std::move(result));
            });
            _queryCallbacks.AddCallback(std::move(query));
            return true;
        }
        catch (std::exception const& exception)
        {
            if (_activeQueries > 0)
                --_activeQueries;
            LOG_ERROR("server-api.characters", "Could not enqueue world database query: {}", exception.what());
            return false;
        }
    }

    void CharactersApi::QueryOrProbe(std::string sql, std::function<void(QueryResult)> callback,
        std::function<void()> onDatabaseUnavailable)
    {
        auto unavailable = onDatabaseUnavailable;
        if (!SubmitQuery(std::move(sql), [this, callback = std::move(callback), unavailable](QueryResult result) mutable
        {
            if (result)
            {
                callback(std::move(result));
                return;
            }

            if (!SubmitQuery("SELECT 1", [callback = std::move(callback), unavailable](QueryResult probe) mutable
            {
                if (probe)
                    callback(nullptr);
                else
                    unavailable();
            }))
                unavailable();
        }))
            unavailable();
    }

    void CharactersApi::ScheduleCallbackPoll()
    {
        if (_stopped)
            return;
        _callbackTimer.expires_after(CallbackPollInterval);
        _callbackTimer.async_wait([this](boost::system::error_code error)
        {
            if (error || _stopped)
                return;
            PollCallbacks();
            ScheduleCallbackPoll();
        });
    }

    void CharactersApi::PollCallbacks()
    {
        _queryCallbacks.ProcessReadyCallbacks();
    }

    bool CharactersApi::List(std::string target, Completion completion)
    {
        CharacterFilters filters;
        if (!ParseFilters(target, filters))
        {
            completion(MakeError(400, "Bad Request", "INVALID_FILTER"));
            return true;
        }
        if (!ReserveRequest())
            return false;

        auto state = std::make_shared<ListState>();
        state->filters = std::move(filters);
        state->completion = std::move(completion);
        state->scanPosition = state->filters.cursor;
        state->botTypes = GetBotTypes();
        for (PlayerSnapshot const& player : GetPlayerSnapshots())
            state->onlineByGuid.emplace(static_cast<uint32_t>(player.guid), player);

        if (state->filters.online == true)
        {
            state->onlineCandidates.reserve(state->onlineByGuid.size());
            for (auto const& [guid, player] : state->onlineByGuid)
            {
                if (guid <= state->scanPosition)
                    continue;
                std::string const botType = BotTypeName(player.accountId, state->botTypes->available,
                    state->botTypes->accountTypes);
                bool const bot = player.bot || IsBotType(botType);
                if (state->filters.mapId && player.mapId != *state->filters.mapId)
                    continue;
                if (state->filters.zoneId && player.zoneId != *state->filters.zoneId)
                    continue;
                if (state->filters.bot && bot != *state->filters.bot)
                    continue;
                if (state->filters.botType && botType != *state->filters.botType)
                    continue;
                state->onlineCandidates.push_back(guid);
            }
            std::sort(state->onlineCandidates.begin(), state->onlineCandidates.end());
        }

        FetchNextListBatch(std::move(state));
        return true;
    }

    void CharactersApi::FetchNextListBatch(std::shared_ptr<ListState> state)
    {
        if (state->finished)
            return;

        auto finish = [this, state](bool hasMore)
        {
            if (state->finished)
                return;
            state->finished = true;

            std::size_t const count = std::min<std::size_t>(state->records.size(), state->filters.limit);
            state->serializedRecords.reserve(count);
            for (std::size_t index = 0; index < count; ++index)
            {
                CharacterRecord const& character = state->records[index];
                auto const runtime = state->onlineByGuid.find(character.guid);
                bool const online = runtime != state->onlineByGuid.end();
                std::string const botType = BotTypeName(character.accountId, state->botTypes->available,
                    state->botTypes->accountTypes);
                bool const bot = (online && runtime->second.bot) || IsBotType(botType);
                uint32_t const mapId = online ? runtime->second.mapId : character.mapId;
                uint32_t const zoneId = online ? runtime->second.zoneId : character.zoneId;
                state->serializedRecords.push_back(BuildCharacterJson(character, online, bot, botType,
                    mapId, zoneId, online ? std::optional<PlayerSnapshot>(runtime->second) : std::nullopt));
            }

            std::optional<uint32_t> cursor;
            if (hasMore)
            {
                if (state->records.size() > state->filters.limit)
                    cursor = state->records[state->filters.limit - 1].guid;
                else
                    cursor = state->scanPosition;
            }
            CompleteRequest(std::move(state->completion), {
                200, "OK", BuildListResponse(state->serializedRecords, hasMore, cursor)});
        };

        if (state->records.size() > state->filters.limit)
        {
            finish(true);
            return;
        }

        if (state->rowsScanned >= MaxRowsScannedPerRequest)
        {
            bool const more = state->filters.online == true
                ? state->onlineCandidateOffset < state->onlineCandidates.size()
                : true;
            finish(more);
            return;
        }

        std::vector<uint32_t> onlineGuids;
        std::vector<uint32_t> const* onlineGuidsPointer = nullptr;
        uint32_t queryCursor = state->scanPosition;
        uint32_t batchSize = std::min(ListBatchSize, MaxRowsScannedPerRequest - state->rowsScanned);
        if (state->filters.online == true)
        {
            if (state->onlineCandidateOffset >= state->onlineCandidates.size())
            {
                finish(false);
                return;
            }
            std::size_t const remaining = state->onlineCandidates.size() - state->onlineCandidateOffset;
            std::size_t const count = std::min<std::size_t>(ListBatchSize, remaining);
            onlineGuids.insert(onlineGuids.end(),
                state->onlineCandidates.begin() + state->onlineCandidateOffset,
                state->onlineCandidates.begin() + state->onlineCandidateOffset + count);
            onlineGuidsPointer = &onlineGuids;
            batchSize = static_cast<uint32_t>(count);
        }

        std::string sql = BuildListSql(state->filters, queryCursor, batchSize, onlineGuidsPointer);
        QueryOrProbe(std::move(sql), [this, state, finish, batchSize, onlineGuids = std::move(onlineGuids)]
            (QueryResult result) mutable
        {
            if (state->filters.online == true)
            {
                if (!onlineGuids.empty())
                {
                    state->onlineCandidateOffset += onlineGuids.size();
                    state->scanPosition = onlineGuids.back();
                    state->rowsScanned += static_cast<uint32_t>(onlineGuids.size());
                }
            }

            uint32_t rowCount = 0;
            if (result)
            {
                do
                {
                    CharacterRecord character = ReadCharacter(result->Fetch(), state->filters.includeTalents);
                    ++rowCount;
                    if (state->filters.online != true)
                    {
                        state->scanPosition = character.guid;
                        ++state->rowsScanned;
                    }

                    auto const runtime = state->onlineByGuid.find(character.guid);
                    bool const online = runtime != state->onlineByGuid.end();
                    std::string const botType = BotTypeName(character.accountId,
                        state->botTypes->available, state->botTypes->accountTypes);
                    bool const bot = (online && runtime->second.bot) || IsBotType(botType);
                    uint32_t const mapId = online ? runtime->second.mapId : character.mapId;
                    uint32_t const zoneId = online ? runtime->second.zoneId : character.zoneId;
                    if ((!state->filters.online || online == *state->filters.online) &&
                        (!state->filters.bot || bot == *state->filters.bot) &&
                        (!state->filters.botType || botType == *state->filters.botType) &&
                        (!state->filters.mapId || mapId == *state->filters.mapId) &&
                        (!state->filters.zoneId || zoneId == *state->filters.zoneId))
                        state->records.push_back(std::move(character));

                    if (state->records.size() > state->filters.limit)
                        break;
                } while (result->NextRow());
            }

            if (state->records.size() > state->filters.limit)
            {
                finish(true);
                return;
            }

            if (state->filters.online == true)
            {
                if (state->onlineCandidateOffset >= state->onlineCandidates.size())
                    finish(false);
                else if (state->rowsScanned >= MaxRowsScannedPerRequest)
                    finish(true);
                else
                    FetchNextListBatch(state);
                return;
            }

            if (rowCount < batchSize)
            {
                finish(false);
                return;
            }
            if (state->rowsScanned >= MaxRowsScannedPerRequest)
            {
                finish(true);
                return;
            }
            FetchNextListBatch(state);
        }, [this, state]
        {
            state->finished = true;
            CompleteRequest(std::move(state->completion), MakeError(503, "Service Unavailable",
                "DATABASE_UNAVAILABLE"));
        });
    }

    bool CharactersApi::Get(uint32_t guid, Completion completion)
    {
        if (guid == 0)
        {
            completion(MakeError(400, "Bad Request", "INVALID_GUID"));
            return true;
        }
        if (!ReserveRequest())
            return false;

        auto state = std::make_shared<ProfileState>();
        state->guid = guid;
        state->completion = std::move(completion);
        state->botTypes = GetBotTypes();
        std::vector<PlayerSnapshot> const players = GetPlayerSnapshots();
        auto const player = std::find_if(players.begin(), players.end(), [guid](PlayerSnapshot const& candidate)
        {
            return candidate.guid == guid;
        });
        if (player != players.end())
        {
            state->runtime = *player;
            state->stats = player->stats;
        }

        std::string const sql = CharacterSelect(
            "c.guid = " + std::to_string(guid) + " AND c.deleteDate IS NULL", true);
        QueryOrProbe(std::move(sql), [this, state](QueryResult result)
        {
            if (!result)
            {
                state->finished = true;
                CompleteRequest(std::move(state->completion), MakeError(404, "Not Found", "CHARACTER_NOT_FOUND"));
                return;
            }

            state->character = ReadCharacter(result->Fetch(), true);
            BeginProfileQueries(state);
        }, [this, state]
        {
            state->finished = true;
            CompleteRequest(std::move(state->completion), MakeError(503, "Service Unavailable",
                "DATABASE_UNAVAILABLE"));
        });
        return true;
    }

    bool CharactersApi::ItemLevels(std::string target, Completion completion)
    {
        CharacterFilters filters;
        if (!ParseFilters(target, filters) ||
            (filters.guids.empty() == filters.exactNames.empty()) ||
            filters.guids.size() > 100 || filters.exactNames.size() > 40)
        {
            completion(MakeError(400, "Bad Request", "INVALID_FILTER"));
            return true;
        }
        if (!ReserveRequest())
            return false;

        auto state = std::make_shared<ItemLevelState>();
        state->completion = std::move(completion);
        std::string filterSql;
        if (!filters.guids.empty())
        {
            state->guids = std::move(filters.guids);
            for (uint32_t guid : state->guids)
                state->itemEntriesByGuid.emplace(guid, std::vector<uint32_t>{});
            for (std::size_t index = 0; index < state->guids.size(); ++index)
            {
                if (index != 0)
                    filterSql += ',';
                filterSql += std::to_string(state->guids[index]);
            }
            filterSql = "c.guid IN (" + filterSql + ')';
        }
        else
        {
            for (std::size_t index = 0; index < filters.exactNames.size(); ++index)
            {
                if (index != 0)
                    filterSql += ',';
                filterSql += SqlString(filters.exactNames[index]);
            }
            filterSql = "c.name IN (" + filterSql + ')';
        }
        std::string const equipmentSql = "SELECT c.guid, c.name, ii.itemEntry FROM characters c "
            "LEFT JOIN character_inventory ci ON ci.guid = c.guid AND ci.bag = 0 "
            "AND ci.slot BETWEEN 0 AND 18 AND ci.slot NOT IN (3, 18) "
            "LEFT JOIN item_instance ii ON ii.guid = ci.item "
            "WHERE c.deleteDate IS NULL AND " + filterSql;

        auto finish = [this, state]
        {
            if (state->finished)
                return;
            state->finished = true;
            std::string body = "{\"data\":[";
            for (std::size_t index = 0; index < state->guids.size(); ++index)
            {
                if (index != 0)
                    body += ',';
                uint64_t total = 0;
                uint32_t count = 0;
                for (uint32_t entry : state->itemEntriesByGuid[state->guids[index]])
                {
                    auto const item = state->itemLevelsByEntry.find(entry);
                    if (item == state->itemLevelsByEntry.end())
                        continue;
                    total += item->second;
                    ++count;
                }
                uint16_t const average = count == 0 ? 0 : static_cast<uint16_t>(std::lround(
                    static_cast<double>(total) / count));
                uint32_t const guid = state->guids[index];
                body += "{\"guid\":" + std::to_string(guid) +
                    ",\"name\":\"" + EscapeJson(state->namesByGuid[guid]) + "\"" +
                    ",\"itemLevel\":" + std::to_string(average) + "}";
            }
            body += "]}";
            CompleteRequest(std::move(state->completion), {200, "OK", std::move(body)});
        };

        QueryOrProbe(std::move(equipmentSql), [this, state, finish](QueryResult result)
        {
            std::vector<uint32_t> entries;
            if (result)
            {
                do
                {
                    Field* fields = result->Fetch();
                    uint32_t const guid = fields[0].Get<uint32>();
                    bool const inserted = state->namesByGuid.emplace(
                        guid, fields[1].Get<std::string>()).second;
                    if (inserted)
                    {
                        if (state->itemEntriesByGuid.find(guid) == state->itemEntriesByGuid.end())
                            state->guids.push_back(guid);
                        state->itemEntriesByGuid.try_emplace(guid, std::vector<uint32_t>{});
                    }
                    if (!fields[2].IsNull())
                    {
                        uint32_t const entry = fields[2].Get<uint32>();
                        state->itemEntriesByGuid[guid].push_back(entry);
                        entries.push_back(entry);
                    }
                } while (result->NextRow());
            }
            std::sort(entries.begin(), entries.end());
            entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
            if (entries.empty())
            {
                finish();
                return;
            }

            std::string entryList;
            for (std::size_t index = 0; index < entries.size(); ++index)
            {
                if (index != 0)
                    entryList += ',';
                entryList += std::to_string(entries[index]);
            }
            std::string const itemSql = "SELECT entry, ItemLevel FROM item_template WHERE entry IN (" +
                entryList + ")";
            if (!SubmitWorldQuery(std::move(itemSql), [state, finish](QueryResult itemResult)
            {
                if (itemResult)
                {
                    do
                    {
                        Field* fields = itemResult->Fetch();
                        state->itemLevelsByEntry.emplace(fields[0].Get<uint32>(),
                            fields[1].Get<uint16>());
                    } while (itemResult->NextRow());
                }
                finish();
            }))
                CompleteRequest(std::move(state->completion), MakeError(503, "Service Unavailable",
                    "WORLD_DATABASE_UNAVAILABLE"));
        }, [this, state]
        {
            state->finished = true;
            CompleteRequest(std::move(state->completion), MakeError(503, "Service Unavailable",
                "CHARACTER_DATABASE_UNAVAILABLE"));
        });
        return true;
    }

    bool CharactersApi::Statistics(Completion completion)
    {
        if (!ReserveRequest())
            return false;
        auto completionState = std::make_shared<Completion>(std::move(completion));
        std::string const sql = "SELECT AVG(latency), "
            "SUM(race IN (1, 3, 4, 7, 11)), SUM(race IN (2, 5, 6, 8, 10)) "
            "FROM characters WHERE online = 1 AND deleteDate IS NULL";
        QueryOrProbe(std::move(sql), [this, completionState](QueryResult result) mutable
        {
            if (!result)
            {
                CompleteRequest(std::move(*completionState), MakeError(503, "Service Unavailable",
                    "DATABASE_UNAVAILABLE"));
                return;
            }
            Field* fields = result->Fetch();
            std::string latency = "null";
            if (!fields[0].IsNull())
                latency = std::to_string(static_cast<int>(std::lround(fields[0].Get<double>())));
            uint64_t const alliance = fields[1].IsNull() ? 0 : fields[1].Get<uint64>();
            uint64_t const horde = fields[2].IsNull() ? 0 : fields[2].Get<uint64>();
            std::string body = "{\"latencyMs\":" + latency +
                ",\"factionCounts\":{\"alliance\":" + std::to_string(alliance) +
                ",\"horde\":" + std::to_string(horde) + "}}";
            CompleteRequest(std::move(*completionState), {200, "OK", std::move(body)});
        }, [this, completionState]() mutable
        {
            CompleteRequest(std::move(*completionState), MakeError(503, "Service Unavailable",
                "DATABASE_UNAVAILABLE"));
        });
        return true;
    }

    bool CharactersApi::InstanceBinds(Completion completion)
    {
        if (!ReserveRequest())
            return false;

        std::string const sql =
            "SELECT c.guid, c.name, c.class, c.level, c.online, c.map, c.instance_id, "
            "i.map, i.difficulty, ci.permanent, ci.extended, i.resettime, i.completedEncounters "
            "FROM character_instance ci "
            "JOIN characters c ON c.guid = ci.guid AND c.deleteDate IS NULL "
            "JOIN instance i ON i.id = ci.instance "
            "ORDER BY i.map, i.difficulty, c.name, c.guid";

        auto completionState = std::make_shared<Completion>(std::move(completion));
        auto runtimeMapByGuid = std::make_shared<std::unordered_map<uint32_t, uint32_t>>();
        for (PlayerSnapshot const& player : GetPlayerSnapshots())
            runtimeMapByGuid->emplace(static_cast<uint32_t>(player.guid), player.mapId);
        QueryOrProbe(std::move(sql), [this, completionState, runtimeMapByGuid](QueryResult result) mutable
        {
            if (!result)
            {
                CompleteRequest(std::move(*completionState), {200, "OK", R"({"data":[]})"});
                return;
            }

            std::string body = R"({"data":[)";
            bool first = true;
            do
            {
                Field* fields = result->Fetch();
                uint32_t const guid = fields[0].Get<uint32_t>();
                std::string const name = fields[1].Get<std::string>();
                uint8_t const playerClass = fields[2].Get<uint8_t>();
                uint8_t const level = fields[3].Get<uint8_t>();
                bool const online = fields[4].Get<uint8_t>() != 0;
                uint32_t currentMap = fields[5].Get<uint32_t>();
                uint32_t const currentInstance = fields[6].Get<uint32_t>();
                uint32_t const mapId = fields[7].Get<uint32_t>();
                if (online)
                {
                    auto const runtimePlayer = runtimeMapByGuid->find(guid);
                    if (runtimePlayer != runtimeMapByGuid->end())
                        currentMap = runtimePlayer->second;
                }
                uint8_t const difficulty = fields[8].Get<uint8_t>();
                bool const permanent = fields[9].Get<uint8_t>() != 0;
                bool const extended = fields[10].Get<uint8_t>() != 0;
                uint64_t const resetAt = fields[11].Get<uint64_t>();
                uint32_t const completedEncounters = fields[12].Get<uint32_t>();

                MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
                bool const isRaid = mapEntry && mapEntry->IsRaid();
                bool const inInstance = online && currentMap == mapId &&
                    (runtimeMapByGuid->find(guid) != runtimeMapByGuid->end() || currentInstance != 0);
                std::string mapName = mapEntry ? mapEntry->name[LOCALE_enUS] : std::string();
                if (!first)
                    body += ',';
                first = false;
                body += Acore::StringFormat(
                    R"({{"guid":{},"name":"{}","class":{},"level":{},"online":{},"mapId":{},"mapName":"{}","isRaid":{},"difficulty":{},"permanent":{},"extended":{},"resetAt":{},"completedEncounters":{},"inInstance":{}}})",
                    guid, EscapeJson(name), playerClass, level, online ? "true" : "false", mapId,
                    EscapeJson(mapName), isRaid ? "true" : "false", difficulty,
                    permanent ? "true" : "false", extended ? "true" : "false", resetAt,
                    completedEncounters, inInstance ? "true" : "false");
            } while (result->NextRow());
            body += "]}";
            CompleteRequest(std::move(*completionState), {200, "OK", std::move(body)});
        }, [this, completionState]() mutable
        {
            CompleteRequest(std::move(*completionState), MakeError(503, "Service Unavailable", "DATABASE_UNAVAILABLE"));
        });
        return true;
    }

    bool CharactersApi::ResetInstanceBinds(std::string target, Completion completion)
    {
        uint32_t mapId = 0;
        uint32_t parsedDifficulty = 0;
        std::string const mapValue = QueryValue(target, "mapId");
        std::string const difficultyValue = QueryValue(target, "difficulty");
        std::string const guidValue = QueryValue(target, "guids");
        std::vector<uint32_t> guids;
        std::unordered_set<uint32_t> uniqueGUIDs;
        std::size_t offset = 0;
        while (offset < guidValue.size())
        {
            std::size_t const separator = guidValue.find(',', offset);
            std::string_view const value(guidValue.data() + offset,
                (separator == std::string::npos ? guidValue.size() : separator) - offset);
            uint32_t guid = 0;
            if (!ParseUnsigned(value, guid) || guid == 0 || !uniqueGUIDs.insert(guid).second || guids.size() >= 100 ||
                (separator != std::string::npos && separator + 1 == guidValue.size()))
            {
                completion(MakeError(400, "Bad Request", "INVALID_GUIDS"));
                return true;
            }
            guids.push_back(guid);
            if (separator == std::string::npos)
                break;
            offset = separator + 1;
        }

        if (!ParseUnsigned(mapValue, mapId) || mapId == 0 ||
            !ParseUnsigned(difficultyValue, parsedDifficulty) || parsedDifficulty >= MAX_DIFFICULTY || guids.empty())
        {
            completion(MakeError(400, "Bad Request", "INVALID_INSTANCE_BIND_RESET"));
            return true;
        }

        Difficulty const difficulty = Difficulty(parsedDifficulty);
        MapEntry const* mapEntry = sMapStore.LookupEntry(mapId);
        if (!mapEntry || !GetMapDifficultyData(mapId, difficulty))
        {
            completion(MakeError(400, "Bad Request", "INVALID_INSTANCE_DIFFICULTY"));
            return true;
        }
        if (mapEntry->IsRaid() ? parsedDifficulty >= MAX_RAID_DIFFICULTY : parsedDifficulty >= MAX_DUNGEON_DIFFICULTY)
        {
            completion(MakeError(400, "Bad Request", "INVALID_INSTANCE_DIFFICULTY"));
            return true;
        }
        if (!ReserveRequest())
            return false;

        auto completionState = std::make_shared<Completion>(std::move(completion));
        bool const queued = GetCommandQueue().Enqueue([this, completionState, guids = std::move(guids), mapId, difficulty]() mutable
        {
            std::string body = R"({"results":[)";
            bool first = true;
            for (uint32_t guid : guids)
            {
                ObjectGuid const playerGuid = ObjectGuid::Create<HighGuid::Player>(guid);
                Player* player = ObjectAccessor::FindConnectedPlayer(playerGuid);
                InstancePlayerBind* bind = sInstanceSaveMgr->PlayerGetBoundInstance(playerGuid, mapId, difficulty);
                std::string status = "reset";
                std::string reason;
                if (!bind)
                {
                    status = "skipped";
                    reason = "Character is no longer bound to this instance.";
                }
                else if (sToCloud9Sidecar->ClusterModeEnabled() && !sToCloud9Sidecar->IsMapAssigned(mapId))
                {
                    status = "skipped";
                    reason = "This worldserver does not own the instance map.";
                }
                else if (player && bind->save && player->GetMapId() == mapId &&
                    player->GetInstanceId() == bind->save->GetInstanceId())
                {
                    // TeleportTo stops combat through the core's normal transfer path.
                    if (!player->TeleportTo(player->m_homebindMapId, player->m_homebindX, player->m_homebindY,
                        player->m_homebindZ, player->GetOrientation()))
                    {
                        status = "skipped";
                        reason = "Could not teleport character to their homebind.";
                    }
                    else
                    {
                        status = "pending";
                        reason = "Character is being teleported home.";
                    }
                }
                else
                {
                    sInstanceSaveMgr->PlayerUnbindInstance(playerGuid, mapId, difficulty, true, player);
                    if (sInstanceSaveMgr->PlayerGetBoundInstance(playerGuid, mapId, difficulty))
                    {
                        status = "failed";
                        reason = "The server could not remove this instance bind.";
                    }
                }

                if (!first)
                    body += ',';
                first = false;
                body += Acore::StringFormat(R"({{"guid":{},"status":"{}","reason":"{}"}})",
                    guid, status, EscapeJson(reason));
            }
            body += "]}";
            ApiResponse response{200, "OK", std::move(body)};
            boost::asio::post(_strand, [this, completionState, response = std::move(response)]() mutable
            {
                CompleteRequest(std::move(*completionState), std::move(response));
            });
        });
        if (!queued)
        {
            CompleteRequest(std::move(*completionState), MakeError(503, "Service Unavailable", "COMMAND_QUEUE_FULL"));
            return true;
        }
        return true;
    }

    void CharactersApi::BeginProfileQueries(std::shared_ptr<ProfileState> state)
    {
        QueryEquipment(std::move(state));
    }

    void CharactersApi::QueryEquipment(std::shared_ptr<ProfileState> state)
    {
        std::string const sql = "SELECT ci.slot, ii.itemEntry, ii.durability, ii.flags, ii.enchantments, ii.guid, ii.randomPropertyId FROM character_inventory ci "
            "INNER JOIN item_instance ii ON ii.guid = ci.item WHERE ci.guid = " +
            std::to_string(state->guid) + " AND ci.bag = 0 AND ci.slot BETWEEN 0 AND 18 ORDER BY ci.slot ASC";
        QueryOrProbe(std::move(sql), [this, state](QueryResult result)
        {
            std::vector<uint32_t> entries;
            if (result)
            {
                do
                {
                    Field* fields = result->Fetch();
                    EquippedItem item;
                    item.slot = fields[0].Get<uint8>();
                    item.itemEntry = fields[1].Get<uint32>();
                    item.durability = fields[2].Get<uint32>();
                    item.soulbound = (fields[3].Get<uint32>() & 1) != 0;
                    item.instanceGuid = fields[5].Get<uint32>();
                    item.randomPropertyId = fields[6].Get<int32>();
                    std::istringstream enchantmentData(fields[4].Get<std::string>());
                    for (uint8_t slot = 0; slot < MAX_ENCHANTMENT_SLOT; ++slot)
                    {
                        uint32_t id = 0;
                        uint32_t durationMs = 0;
                        uint32_t charges = 0;
                        if (!(enchantmentData >> id >> durationMs >> charges))
                            break;
                        if (id == 0)
                            continue;

                        std::string name;
                        std::string nameRu;
                        if (SpellItemEnchantmentEntry const* entry = sSpellItemEnchantmentStore.LookupEntry(id))
                        {
                            if (entry->description[LOCALE_enUS])
                                name = entry->description[LOCALE_enUS];
                            if (entry->description[LOCALE_ruRU])
                                nameRu = entry->description[LOCALE_ruRU];
                        }
                        if (nameRu.empty())
                            nameRu = name;
                        EquipmentEnchantment enchantment;
                        enchantment.slot = slot;
                        enchantment.id = id;
                        enchantment.durationMs = durationMs;
                        enchantment.charges = charges;
                        enchantment.name = std::move(name);
                        enchantment.nameRu = std::move(nameRu);
                        if (SpellItemEnchantmentEntry const* entry = sSpellItemEnchantmentStore.LookupEntry(id))
                        {
                            enchantment.gemId = entry->GemID;
                            for (std::size_t effectIndex = 0; effectIndex < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++effectIndex)
                            {
                                uint32_t const type = entry->type[effectIndex];
                                uint32_t const amount = entry->amount[effectIndex];
                                uint32_t const spellId = entry->spellid[effectIndex];
                                if (type != 0 || amount != 0 || spellId != 0)
                                    enchantment.effects.push_back({type, amount, spellId});
                            }
                        }
                        if (SpellEnchantProcEntry const* proc = sSpellMgr->GetSpellEnchantProcEvent(id))
                        {
                            enchantment.hasProc = true;
                            enchantment.proc = BuildEnchantmentProc(*proc);
                        }
                        item.enchantments.push_back(std::move(enchantment));
                    }
                    state->equipment.push_back(std::move(item));
                    entries.push_back(fields[1].Get<uint32>());
                } while (result->NextRow());
            }
            std::sort(entries.begin(), entries.end());
            entries.erase(std::unique(entries.begin(), entries.end()), entries.end());
            if (entries.empty())
            {
                QueryStats(std::move(state));
                return;
            }

            std::string entryList;
            for (std::size_t index = 0; index < entries.size(); ++index)
            {
                if (index != 0)
                    entryList += ',';
                entryList += std::to_string(entries[index]);
            }
            std::string const itemSql = "SELECT item_template.entry, item_template.displayid, item_template.name, Quality, ItemLevel, RequiredLevel, "
                "InventoryType, class, subclass, Flags, MaxCount, bonding, armor, block, delay, MaxDurability, "
                "ItemLimitCategory, stat_type1, stat_value1, stat_type2, stat_value2, stat_type3, stat_value3, "
                "stat_type4, stat_value4, stat_type5, stat_value5, stat_type6, stat_value6, stat_type7, "
                "stat_value7, stat_type8, stat_value8, stat_type9, stat_value9, stat_type10, stat_value10, "
                "dmg_min1, dmg_max1, dmg_type1, dmg_min2, dmg_max2, dmg_type2, holy_res, fire_res, "
                "nature_res, frost_res, shadow_res, arcane_res, "
                "COALESCE(NULLIF(item_template_locale.Name, ''), item_template.name) "
                ",sellprice, allowableclass, allowablerace, requiredskill, requiredskillrank, requiredspell, "
                "requiredhonorrank, requiredcityrank, requiredreputationfaction, requiredreputationrank, "
                "RangedModRange, ItemSet, socketColor_1, socketContent_1, socketColor_2, socketContent_2, "
                "socketColor_3, socketContent_3, socketBonus, GemProperties, "
                "spellid_1, spelltrigger_1, spellcharges_1, spellppmRate_1, spellcooldown_1, spellcategory_1, spellcategorycooldown_1, "
                "spellid_2, spelltrigger_2, spellcharges_2, spellppmRate_2, spellcooldown_2, spellcategory_2, spellcategorycooldown_2, "
                "spellid_3, spelltrigger_3, spellcharges_3, spellppmRate_3, spellcooldown_3, spellcategory_3, spellcategorycooldown_3, "
                "spellid_4, spelltrigger_4, spellcharges_4, spellppmRate_4, spellcooldown_4, spellcategory_4, spellcategorycooldown_4, "
                "spellid_5, spelltrigger_5, spellcharges_5, spellppmRate_5, spellcooldown_5, spellcategory_5, spellcategorycooldown_5 "
                "FROM item_template LEFT JOIN item_template_locale ON item_template_locale.ID = item_template.entry "
                "AND item_template_locale.locale = 'ruRU' WHERE item_template.entry IN (" + entryList + ")";
            if (!SubmitWorldQuery(std::move(itemSql), [this, state](QueryResult itemResult)
            {
                if (!itemResult)
                {
                    if (!SubmitWorldQuery("SELECT 1", [this, state](QueryResult probe)
                    {
                        if (probe)
                        {
                            QueryStats(std::move(state));
                            return;
                        }
                        state->finished = true;
                        CompleteRequest(std::move(state->completion), MakeError(503, "Service Unavailable",
                            "WORLD_DATABASE_UNAVAILABLE"));
                    }))
                    {
                        state->finished = true;
                        CompleteRequest(std::move(state->completion), MakeError(503, "Service Unavailable",
                            "WORLD_DATABASE_UNAVAILABLE"));
                    }
                    return;
                }

                do
                {
                    Field* fields = itemResult->Fetch();
                    uint32_t const entry = fields[0].Get<uint32>();
                    for (EquippedItem& item : state->equipment)
                    {
                        if (item.itemEntry != entry)
                            continue;
                        item.displayId = fields[1].Get<uint32>();
                        item.name = fields[2].Get<std::string>();
                        item.quality = fields[3].Get<uint8>();
                        item.itemLevel = fields[4].Get<uint16>();
                        item.requiredLevel = fields[5].Get<uint8>();
                        item.inventoryType = fields[6].Get<uint8>();
                        item.itemClass = fields[7].Get<uint8>();
                        item.itemSubclass = fields[8].Get<uint8>();
                        item.flags = fields[9].Get<uint32>();
                        item.maxCount = fields[10].Get<int32>();
                        item.bonding = fields[11].Get<uint8>();
                        item.armor = fields[12].Get<uint32>();
                        item.block = fields[13].Get<uint32>();
                        item.delayMs = fields[14].Get<uint32>();
                        item.maxDurability = fields[15].Get<uint32>();
                        item.itemLimitCategory = fields[16].Get<int16>();
                        for (std::size_t statIndex = 0; statIndex < 10; ++statIndex)
                        {
                            uint8_t const type = fields[17 + statIndex * 2].Get<uint8>();
                            int32_t const value = fields[18 + statIndex * 2].Get<int32>();
                            if (value != 0)
                                item.stats.push_back({type, value});
                        }
                        for (std::size_t damageIndex = 0; damageIndex < 2; ++damageIndex)
                        {
                            std::size_t const fieldIndex = 37 + damageIndex * 3;
                            float const min = fields[fieldIndex].Get<float>();
                            float const max = fields[fieldIndex + 1].Get<float>();
                            if (max > 0.0f)
                                item.damage.push_back({min, max, fields[fieldIndex + 2].Get<uint8>()});
                        }
                        for (std::size_t resistanceIndex = 0; resistanceIndex < item.resistances.size();
                            ++resistanceIndex)
                            item.resistances[resistanceIndex] = fields[43 + resistanceIndex].Get<int32>();
                        item.nameRu = fields[49].Get<std::string>();
                        item.sellPrice = fields[50].Get<uint32>();
                        item.allowableClass = fields[51].Get<uint32>();
                        item.allowableRace = fields[52].Get<uint32>();
                        item.requiredSkill = fields[53].Get<uint32>();
                        item.requiredSkillRank = fields[54].Get<uint32>();
                        item.requiredSpell = fields[55].Get<uint32>();
                        item.requiredHonorRank = fields[56].Get<uint32>();
                        item.requiredCityRank = fields[57].Get<uint32>();
                        item.requiredReputationFaction = fields[58].Get<uint32>();
                        item.requiredReputationRank = fields[59].Get<uint32>();
                        item.rangedModRange = fields[60].Get<float>();
                        item.itemSetId = fields[61].Get<uint32>();
                        for (std::size_t socketIndex = 0; socketIndex < item.sockets.size(); ++socketIndex)
                        {
                            std::size_t const fieldIndex = 62 + socketIndex * 2;
                            item.sockets[socketIndex] = {fields[fieldIndex].Get<uint32>(), fields[fieldIndex + 1].Get<uint32>()};
                        }
                        item.socketBonus = fields[68].Get<uint32>();
                        item.gemProperties = fields[69].Get<uint32>();
                        for (std::size_t spellIndex = 0; spellIndex < MAX_ITEM_PROTO_SPELLS; ++spellIndex)
                        {
                            std::size_t const fieldIndex = 70 + spellIndex * 7;
                            int32_t const spellId = fields[fieldIndex].Get<int32>();
                            if (spellId == 0)
                                continue;
                            EquipmentSpell spell;
                            spell.id = spellId;
                            spell.trigger = fields[fieldIndex + 1].Get<uint8>();
                            spell.charges = fields[fieldIndex + 2].Get<int16>();
                            spell.ppmRate = fields[fieldIndex + 3].Get<float>();
                            spell.cooldownMs = fields[fieldIndex + 4].Get<int32>();
                            spell.category = fields[fieldIndex + 5].Get<uint16>();
                            spell.categoryCooldownMs = fields[fieldIndex + 6].Get<int32>();
                            if (SpellEntry const* spellEntry = sSpellStore.LookupEntry(static_cast<uint32_t>(spellId)))
                            {
                                spell.name = spellEntry->SpellName[LOCALE_enUS] ? spellEntry->SpellName[LOCALE_enUS] : "";
                                spell.nameRu = spellEntry->SpellName[LOCALE_ruRU] ? spellEntry->SpellName[LOCALE_ruRU] : spell.name;
                            }
                            if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(static_cast<uint32_t>(spellId)))
                            {
                                for (uint8_t effectIndex = 0; effectIndex < MAX_SPELL_EFFECTS; ++effectIndex)
                                {
                                    SpellEffectInfo const& source = spellInfo->Effects[effectIndex];
                                    if (!source.IsEffect())
                                        continue;
                                    spell.effects.push_back({source.EffectIndex, source.Effect,
                                        static_cast<uint32_t>(source.ApplyAuraName), source.BasePoints,
                                        source.DieSides, source.RealPointsPerLevel, source.TriggerSpell,
                                        source.MiscValue, source.MiscValueB, source.Amplitude});
                                }
                            }
                            if (SpellProcEntry const* proc = sSpellMgr->GetSpellProcEntry(static_cast<uint32_t>(spellId)))
                            {
                                spell.hasProc = true;
                                spell.proc = BuildSpellProc(*proc);
                            }
                            item.spells.push_back(std::move(spell));
                        }
                        if (item.itemSetId != 0)
                        {
                            if (ItemSetEntry const* set = sItemSetStore.LookupEntry(item.itemSetId))
                            {
                                item.itemSetName = set->name[LOCALE_enUS] ? set->name[LOCALE_enUS] : "";
                                item.itemSetNameRu = set->name[LOCALE_ruRU] ? set->name[LOCALE_ruRU] : item.itemSetName;
                                for (std::size_t bonusIndex = 0; bonusIndex < MAX_ITEM_SET_SPELLS; ++bonusIndex)
                                {
                                    uint32_t const spellId = set->spells[bonusIndex];
                                    uint32_t const requiredItems = set->items_to_triggerspell[bonusIndex];
                                    if (spellId == 0 || requiredItems == 0)
                                        continue;
                                    EquipmentSetBonus bonus;
                                    bonus.requiredItems = static_cast<uint8_t>(requiredItems);
                                    bonus.spellId = spellId;
                                    if (SpellEntry const* spell = sSpellStore.LookupEntry(spellId))
                                    {
                                        bonus.spellName = spell->SpellName[LOCALE_enUS] ? spell->SpellName[LOCALE_enUS] : "";
                                        bonus.spellNameRu = spell->SpellName[LOCALE_ruRU] ? spell->SpellName[LOCALE_ruRU] : bonus.spellName;
                                    }
                                    if (SpellInfo const* spellInfo = sSpellMgr->GetSpellInfo(spellId))
                                    {
                                        for (uint8_t effectIndex = 0; effectIndex < MAX_SPELL_EFFECTS; ++effectIndex)
                                        {
                                            SpellEffectInfo const& source = spellInfo->Effects[effectIndex];
                                            if (!source.IsEffect())
                                                continue;
                                            bonus.effects.push_back({source.EffectIndex, source.Effect,
                                                static_cast<uint32_t>(source.ApplyAuraName), source.BasePoints,
                                                source.DieSides, source.RealPointsPerLevel, source.TriggerSpell,
                                                source.MiscValue, source.MiscValueB, source.Amplitude});
                                        }
                                    }
                                    if (SpellProcEntry const* proc = sSpellMgr->GetSpellProcEntry(spellId))
                                    {
                                        bonus.hasProc = true;
                                        bonus.proc = BuildSpellProc(*proc);
                                    }
                                    item.itemSetBonuses.push_back(std::move(bonus));
                                }
                            }
                        }
                        if (item.socketBonus != 0)
                        {
                            if (SpellItemEnchantmentEntry const* enchantment = sSpellItemEnchantmentStore.LookupEntry(item.socketBonus))
                            {
                                item.socketBonusName = enchantment->description[LOCALE_enUS] ? enchantment->description[LOCALE_enUS] : "";
                                item.socketBonusNameRu = enchantment->description[LOCALE_ruRU] ? enchantment->description[LOCALE_ruRU] : item.socketBonusName;
                                for (std::size_t effectIndex = 0; effectIndex < MAX_SPELL_ITEM_ENCHANTMENT_EFFECTS; ++effectIndex)
                                {
                                    uint32_t const type = enchantment->type[effectIndex];
                                    uint32_t const amount = enchantment->amount[effectIndex];
                                    uint32_t const spellId = enchantment->spellid[effectIndex];
                                    if (type != 0 || amount != 0 || spellId != 0)
                                        item.socketBonusEffects.push_back({type, amount, spellId});
                                }
                            }
                            if (SpellEnchantProcEntry const* proc = sSpellMgr->GetSpellEnchantProcEvent(item.socketBonus))
                            {
                                item.hasSocketBonusProc = true;
                                item.socketBonusProc = BuildEnchantmentProc(*proc);
                            }
                        }
                        if (item.randomPropertyId > 0)
                        {
                            if (ItemRandomPropertiesEntry const* property = sItemRandomPropertiesStore.LookupEntry(static_cast<uint32_t>(item.randomPropertyId)))
                            {
                                item.randomPropertyName = property->Name[LOCALE_enUS] ? property->Name[LOCALE_enUS] : "";
                                item.randomPropertyNameRu = property->Name[LOCALE_ruRU] ? property->Name[LOCALE_ruRU] : item.randomPropertyName;
                                for (uint32_t enchantmentId : property->Enchantment)
                                    if (enchantmentId != 0)
                                        item.randomPropertyEnchantments.push_back(BuildRandomPropertyEnchantment(enchantmentId, 0));
                            }
                        }
                        else if (item.randomPropertyId < 0)
                        {
                            uint32_t const suffixId = static_cast<uint32_t>(-static_cast<int64_t>(item.randomPropertyId));
                            if (ItemRandomSuffixEntry const* suffix = sItemRandomSuffixStore.LookupEntry(suffixId))
                            {
                                item.randomPropertyName = suffix->Name[LOCALE_enUS] ? suffix->Name[LOCALE_enUS] : "";
                                item.randomPropertyNameRu = suffix->Name[LOCALE_ruRU] ? suffix->Name[LOCALE_ruRU] : item.randomPropertyName;
                                for (std::size_t enchantmentIndex = 0; enchantmentIndex < suffix->Enchantment.size(); ++enchantmentIndex)
                                    if (suffix->Enchantment[enchantmentIndex] != 0)
                                        item.randomPropertyEnchantments.push_back(BuildRandomPropertyEnchantment(
                                            suffix->Enchantment[enchantmentIndex], suffix->AllocationPct[enchantmentIndex]));
                            }
                        }
                        item.unknownTemplate = false;
                    }
                } while (itemResult->NextRow());
                QueryStats(std::move(state));
            }))
            {
                state->finished = true;
                CompleteRequest(std::move(state->completion), MakeError(503, "Service Unavailable",
                    "WORLD_DATABASE_UNAVAILABLE"));
            }
        }, [this, state]
        {
            state->finished = true;
            CompleteRequest(std::move(state->completion), MakeError(503, "Service Unavailable",
                "DATABASE_UNAVAILABLE"));
        });
    }

    void CharactersApi::QueryStats(std::shared_ptr<ProfileState> state)
    {
        if (state->runtime)
        {
            QueryProfessions(std::move(state));
            return;
        }

        std::string const sql = "SELECT maxhealth, maxpower1, maxpower2, maxpower3, maxpower4, "
            "maxpower5, maxpower6, maxpower7, strength, agility, stamina, intellect, spirit, armor, "
            "resHoly, resFire, resNature, resFrost, resShadow, resArcane, blockPct, dodgePct, parryPct, "
            "critPct, rangedCritPct, spellCritPct, attackPower, rangedAttackPower, spellPower, resilience "
            "FROM character_stats WHERE guid = " + std::to_string(state->guid);
        QueryOrProbe(std::move(sql), [this, state](QueryResult result)
        {
            if (result)
            {
                Field* fields = result->Fetch();
                CharacterStatSnapshot stats;
                stats.maxHealth = fields[0].Get<uint32>();
                for (std::size_t index = 0; index < stats.maxPowers.size(); ++index)
                    stats.maxPowers[index] = fields[1 + index].Get<uint32>();
                for (std::size_t index = 0; index < stats.primaryAttributes.size(); ++index)
                    stats.primaryAttributes[index] = fields[8 + index].Get<uint32>();
                stats.armor = fields[13].Get<uint32>();
                for (std::size_t index = 0; index < stats.resistances.size(); ++index)
                    stats.resistances[index] = fields[14 + index].Get<uint32>();
                stats.blockPct = fields[20].Get<float>();
                stats.dodgePct = fields[21].Get<float>();
                stats.parryPct = fields[22].Get<float>();
                stats.critPct = fields[23].Get<float>();
                stats.rangedCritPct = fields[24].Get<float>();
                stats.spellCritPct = fields[25].Get<float>();
                stats.attackPower = fields[26].Get<uint32>();
                stats.rangedAttackPower = fields[27].Get<uint32>();
                stats.spellPower = fields[28].Get<uint32>();
                stats.resilience = fields[29].Get<uint32>();
                state->stats = stats;
            }
            QueryProfessions(std::move(state));
        }, [this, state]
        {
            state->stats.reset();
            QueryProfessions(std::move(state));
        });
    }

    void CharactersApi::QueryProfessions(std::shared_ptr<ProfileState> state)
    {
        std::string const sql = "SELECT skill, value, `max` FROM character_skills WHERE guid = " +
            std::to_string(state->guid) + " AND skill IN (171,164,333,202,182,773,755,165,186,393,197,185,129,356) "
            "ORDER BY skill ASC";
        QueryOrProbe(std::move(sql), [this, state](QueryResult result)
        {
            if (result)
            {
                do
                {
                    Field* fields = result->Fetch();
                    state->professions.push_back({fields[0].Get<uint16>(), fields[1].Get<uint16>(),
                        fields[2].Get<uint16>()});
                } while (result->NextRow());
            }

            bool const online = state->runtime.has_value();
            std::string const botType = BotTypeName(state->character.accountId, state->botTypes->available,
                state->botTypes->accountTypes);
            bool const bot = (online && state->runtime->bot) || IsBotType(botType);
            uint32_t const mapId = online ? state->runtime->mapId : state->character.mapId;
            uint32_t const zoneId = online ? state->runtime->zoneId : state->character.zoneId;
            state->finished = true;
            CompleteRequest(std::move(state->completion), {200, "OK", BuildProfileJson(state->character,
                online, bot, botType, mapId, zoneId, state->runtime, state->stats,
                state->equipment, state->professions)});
        }, [this, state]
        {
            state->finished = true;
            CompleteRequest(std::move(state->completion), MakeError(503, "Service Unavailable",
                "DATABASE_UNAVAILABLE"));
        });
    }

    std::shared_ptr<CharactersApi::BotTypeSnapshot const> CharactersApi::GetBotTypes() const
    {
        std::lock_guard lock(_botTypesMutex);
        return _botTypes;
    }

    void CharactersApi::StartBotTypeWorker()
    {
#ifdef MOD_PLAYERBOTS
        {
            std::lock_guard lock(_botWorkerMutex);
            _stopBotWorker = false;
        }
        try
        {
            _botWorker = std::thread([this]
            {
                while (true)
                {
                    {
                        std::lock_guard lock(_botWorkerMutex);
                        if (_stopBotWorker)
                            return;
                    }

                    try
                    {
                        RefreshBotTypes();
                    }
                    catch (std::exception const& exception)
                    {
                        LOG_WARN("server-api.characters", "Playerbots account type refresh failed: {}",
                            exception.what());
                        std::lock_guard lock(_botTypesMutex);
                        _botTypes = std::make_shared<BotTypeSnapshot>();
                    }
                    catch (...)
                    {
                        LOG_WARN("server-api.characters", "Playerbots account type refresh failed");
                        std::lock_guard lock(_botTypesMutex);
                        _botTypes = std::make_shared<BotTypeSnapshot>();
                    }

                    bool const available = GetBotTypes()->available;
                    auto const interval = available ? BotTypeRefreshInterval : BotTypeRetryInterval;
                    std::unique_lock lock(_botWorkerMutex);
                    if (_botWorkerCondition.wait_for(lock, interval,
                        [this] { return _stopBotWorker; }))
                        return;
                }
            });
        }
        catch (std::system_error const& exception)
        {
            LOG_WARN("server-api.characters", "Could not start Playerbots account type worker: {}",
                exception.what());
        }
#endif
    }

    void CharactersApi::StopBotTypeWorker()
    {
#ifdef MOD_PLAYERBOTS
        {
            std::lock_guard lock(_botWorkerMutex);
            _stopBotWorker = true;
        }
        _botWorkerCondition.notify_all();
        if (_botWorker.joinable())
            _botWorker.join();
#endif
    }

    void CharactersApi::RefreshBotTypes()
    {
#ifdef MOD_PLAYERBOTS
        auto next = std::make_shared<BotTypeSnapshot>();
        if (QueryResult result = PlayerbotsDatabase.Query(
            "SELECT account_id, account_type FROM playerbots_account_type ORDER BY account_id"))
        {
            next->available = true;
            do
            {
                Field* fields = result->Fetch();
                next->accountTypes.emplace(fields[0].Get<uint32>(), fields[1].Get<uint8>());
            } while (result->NextRow());
        }
        else if (QueryResult countResult = PlayerbotsDatabase.Query("SELECT COUNT(*) FROM playerbots_account_type"))
        {
            // The table exists but is empty; missing rows mean a regular player account.
            next->available = true;
        }

        {
            std::lock_guard lock(_botTypesMutex);
            _botTypes = std::move(next);
        }
#endif
    }
}
