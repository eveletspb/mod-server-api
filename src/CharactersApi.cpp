/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/CharactersApi.h"

#include "ServerApi/HttpUtils.h"
#include "ServerApi/ServerSnapshot.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "QueryResult.h"

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
#include <string_view>
#include <system_error>
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
        };

        struct EquippedItem
        {
            uint8_t slot = 0;
            uint32_t itemEntry = 0;
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

            static constexpr std::array<std::string_view, 15> AllowedKeys = {
                "limit", "cursor", "name", "accountId", "online", "bot", "botType", "race",
                "class", "minLevel", "maxLevel", "guildId", "mapId", "zoneId", "includeDeleted"
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
                {"bot", &filters.bot}, {"includeDeleted", nullptr}})
            {
                auto const value = values.find(std::string(key));
                if (value == values.end())
                    continue;
                bool parsed = false;
                if (!ParseBoolean(value->second, parsed))
                    return false;
                if (output)
                    *output = parsed;
                else
                    filters.includeDeleted = parsed;
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

        CharacterRecord ReadCharacter(Field* fields)
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
            return character;
        }

        std::string CharacterSelect(std::string_view where)
        {
            return "SELECT c.guid, COALESCE(NULLIF(c.account, 0), c.deleteInfos_Account, 0), "
                "COALESCE(NULLIF(c.name COLLATE utf8mb4_unicode_ci, ''), "
                "c.deleteInfos_Name COLLATE utf8mb4_unicode_ci, ''), c.race, c.class, c.gender, c.level, "
                "c.xp, c.money, COALESCE(gm.guildid, 0), COALESCE(g.name, ''), c.map, c.zone, c.deleteDate, "
                "c.position_x, c.position_y, c.position_z, c.orientation, "
                "c.activeTalentGroup, c.talentGroupsCount "
                "FROM characters c "
                "LEFT JOIN guild_member gm ON gm.guid = c.guid "
                "LEFT JOIN guild g ON g.guildid = gm.guildid WHERE " + std::string(where);
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
            if (!filters.namePrefix.empty())
                where += " AND COALESCE(NULLIF(c.name COLLATE utf8mb4_unicode_ci, ''), "
                    "c.deleteInfos_Name COLLATE utf8mb4_unicode_ci, '') LIKE " +
                    SqlString(NamePattern(filters.namePrefix)) +
                    " COLLATE utf8mb4_unicode_ci ESCAPE '='";
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

            return CharacterSelect(where) + " ORDER BY c.guid ASC LIMIT " + std::to_string(batchSize);
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
            return "{\"guid\":" + std::to_string(character.guid) +
                ",\"accountId\":" + std::to_string(character.accountId) +
                ",\"name\":\"" + EscapeJson(character.name) + "\"" +
                ",\"race\":" + std::to_string(character.race) +
                ",\"class\":" + std::to_string(character.playerClass) +
                ",\"gender\":" + std::to_string(character.gender) +
                ",\"level\":" + std::to_string(character.level) +
                ",\"guildId\":" + std::to_string(character.guildId) +
                ",\"guildName\":\"" + EscapeJson(character.guildName) + "\"" +
                ",\"online\":" + (online ? "true" : "false") +
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

        std::string BuildProfileJson(CharacterRecord const& character, bool online, bool bot,
            std::string_view botType, uint32_t mapId, uint32_t zoneId,
            std::optional<PlayerSnapshot> const& runtime, std::vector<EquippedItem> const& equipment,
            std::vector<ProfessionSkill> const& professions)
        {
            std::string body = BuildCharacterJson(character, online, bot, botType, mapId, zoneId, runtime);
            body.pop_back();
            body += ",\"xp\":" + std::to_string(character.xp) +
                ",\"money\":" + std::to_string(character.money) +
                ",\"talents\":{\"activeGroup\":" + std::to_string(character.activeTalentGroup) +
                ",\"totalGroups\":" + std::to_string(character.talentGroupsCount) + "},\"equipment\":[";
            for (std::size_t index = 0; index < equipment.size(); ++index)
            {
                if (index != 0)
                    body += ',';
                body += "{\"slot\":" + std::to_string(equipment[index].slot) +
                    ",\"itemEntry\":" + std::to_string(equipment[index].itemEntry) + "}";
            }
            body += "],\"professions\":[";
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
        bool finished = false;
    };

    CharactersApi::CharactersApi(boost::asio::io_context& ioContext)
        : _ioContext(ioContext), _callbackTimer(ioContext), _botTypes(std::make_shared<BotTypeSnapshot>()) { }

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
                    CharacterRecord character = ReadCharacter(result->Fetch());
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
            state->runtime = *player;

        std::string const sql = CharacterSelect("c.guid = " + std::to_string(guid) + " AND c.deleteDate IS NULL");
        QueryOrProbe(std::move(sql), [this, state](QueryResult result)
        {
            if (!result)
            {
                state->finished = true;
                CompleteRequest(std::move(state->completion), MakeError(404, "Not Found", "CHARACTER_NOT_FOUND"));
                return;
            }

            state->character = ReadCharacter(result->Fetch());
            BeginProfileQueries(state);
        }, [this, state]
        {
            state->finished = true;
            CompleteRequest(std::move(state->completion), MakeError(503, "Service Unavailable",
                "DATABASE_UNAVAILABLE"));
        });
        return true;
    }

    void CharactersApi::BeginProfileQueries(std::shared_ptr<ProfileState> state)
    {
        QueryEquipment(std::move(state));
    }

    void CharactersApi::QueryEquipment(std::shared_ptr<ProfileState> state)
    {
        std::string const sql = "SELECT ci.slot, ii.itemEntry FROM character_inventory ci "
            "INNER JOIN item_instance ii ON ii.guid = ci.item WHERE ci.guid = " +
            std::to_string(state->guid) + " AND ci.bag = 0 AND ci.slot BETWEEN 0 AND 18 ORDER BY ci.slot ASC";
        QueryOrProbe(std::move(sql), [this, state](QueryResult result)
        {
            if (result)
            {
                do
                {
                    Field* fields = result->Fetch();
                    state->equipment.push_back({fields[0].Get<uint8>(), fields[1].Get<uint32>()});
                } while (result->NextRow());
            }
            QueryProfessions(std::move(state));
        }, [this, state]
        {
            state->finished = true;
            CompleteRequest(std::move(state->completion), MakeError(503, "Service Unavailable",
                "DATABASE_UNAVAILABLE"));
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
                online, bot, botType, mapId, zoneId, state->runtime, state->equipment, state->professions)});
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
