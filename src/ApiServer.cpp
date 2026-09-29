/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/ApiServer.h"
#include "ServerApi/CommandQueue.h"
#include "ServerApi/EventBus.h"
#include "ServerApi/HttpUtils.h"
#include "ServerApi/ModuleRegistry.h"
#include "ServerApi/ServerSnapshot.h"

#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectGuid.h"
#include "Player.h"
#include "StringFormat.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <sstream>
#include <string_view>
#include <utility>
#include <vector>

#include <boost/asio/post.hpp>
#include <boost/asio/ip/address.hpp>
#include <boost/asio/write.hpp>
#include <boost/beast/core.hpp>
#include <boost/beast/http.hpp>
#include <boost/beast/websocket.hpp>

namespace ServerApi
{
    namespace
    {
        std::string BuildResponse(uint16_t status, std::string_view reason, std::string_view body,
            std::string_view headers = {})
        {
            return Acore::StringFormat(
                "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\n"
                "Connection: close\r\n{}\r\n{}",
                status, reason, body.size(), headers, body);
        }

        bool HasValidBearerToken(std::string const& request, Config const& config)
        {
            if (!config.authEnabled)
                return true;

            if (config.apiKey.empty())
                return false;

            static constexpr std::string_view BearerPrefix = "Bearer ";
            std::string const authorization = HeaderValue(request, "authorization");
            if (!authorization.starts_with(BearerPrefix))
                return false;

            std::string_view const token(authorization.data() + BearerPrefix.size(),
                authorization.size() - BearerPrefix.size());
            if (token.size() != config.apiKey.size())
                return false;

            unsigned char difference = 0;
            for (std::size_t index = 0; index < token.size(); ++index)
            {
                difference |= static_cast<unsigned char>(token[index]) ^
                    static_cast<unsigned char>(config.apiKey[index]);
            }
            return difference == 0;
        }

        std::string BuildServerResponse()
        {
            ServerSnapshot const snapshot = GetServerSnapshot();
            return Acore::StringFormat(
                R"({{"realmId":{},"serverTime":{},"uptimeSeconds":{},"playersOnline":{},"botsOnline":{}}})",
                snapshot.realmId, snapshot.serverTime, snapshot.uptimeSeconds,
                snapshot.playersOnline, snapshot.botsOnline);
        }

        std::string BuildMetricsResponse()
        {
            ServerSnapshot const snapshot = GetServerSnapshot();
            return Acore::StringFormat(
                R"({{"activeMaps":{},"activeInstances":{},"playersOnline":{},"botsOnline":{},)"
                R"("eventQueue":{},"commandQueue":{},"droppedEvents":{}}})",
                snapshot.activeMaps, snapshot.activeInstances, snapshot.playersOnline, snapshot.botsOnline,
                GetEventBus().QueueSize(), GetCommandQueue().Size(), GetEventBus().DroppedCount());
        }

        std::string BuildModulesResponse()
        {
            std::string body = R"({"data":[)";
            bool first = true;
            for (ModuleDescriptor const& module : GetModuleRegistry().Snapshot())
            {
                if (!first)
                    body += ',';
                first = false;
                body += R"({"name":")" + EscapeJson(module.name) +
                    R"(","version":")" + EscapeJson(module.version) + R"(","capabilities":[)";
                for (std::size_t index = 0; index < module.capabilities.size(); ++index)
                {
                    if (index > 0)
                        body += ',';
                    body += '"' + EscapeJson(module.capabilities[index]) + '"';
                }
                body += "]}";
            }
            body += "]}";
            return body;
        }

        std::string BuildPlayerJson(PlayerSnapshot const& player, bool details)
        {
            if (!details)
            {
                return Acore::StringFormat(
                    R"({{"guid":{},"name":"{}","level":{},"class":{},"race":{},"mapId":{},"zoneId":{},"online":true}})",
                    player.guid, EscapeJson(player.name), player.level, player.playerClass,
                    player.race, player.mapId, player.zoneId);
            }

            return Acore::StringFormat(
                R"({{"guid":{},"name":"{}","level":{},"class":{},"race":{},"mapId":{},"zoneId":{},)"
                R"("online":true,"health":{{"current":{},"max":{}}},)"
                R"("power":{{"current":{},"max":{}}},)"
                R"("position":{{"mapId":{},"x":{},"y":{},"z":{},"orientation":{}}}}})",
                player.guid, EscapeJson(player.name), player.level, player.playerClass, player.race,
                player.mapId, player.zoneId, player.health, player.maxHealth, player.power,
                player.maxPower, player.mapId, player.x, player.y, player.z, player.orientation);
        }

        std::string BuildPlayersResponse(std::string const& target)
        {
            std::vector<PlayerSnapshot> players = GetPlayerSnapshots();
            std::string const name = QueryValue(target, "name");

            uint32_t mapId = 0;
            std::string const mapIdValue = QueryValue(target, "mapId");
            bool const hasMapFilter = !mapIdValue.empty();
            if (hasMapFilter && !ParseUnsigned(mapIdValue, mapId))
                return R"({"error":{"code":"INVALID_MAP_ID"}})";

            uint32_t limit = 100;
            std::string const limitValue = QueryValue(target, "limit");
            if (!limitValue.empty() && (!ParseUnsigned(limitValue, limit) || limit == 0))
                return R"({"error":{"code":"INVALID_LIMIT"}})";
            limit = std::min(limit, 1000u);

            std::string body = R"({"data":[)";
            uint32_t included = 0;
            for (PlayerSnapshot const& player : players)
            {
                if (hasMapFilter && player.mapId != mapId)
                    continue;
                if (!name.empty() && player.name.find(name) == std::string::npos)
                    continue;
                if (included++ >= limit)
                    break;
                if (included > 1)
                    body += ',';
                body += BuildPlayerJson(player, false);
            }
            body += "]}";
            return body;
        }

        std::string BuildBotsResponse()
        {
            std::string body = R"({"data":[)";
            std::vector<PlayerSnapshot> const players = GetPlayerSnapshots();
            bool first = true;
            for (PlayerSnapshot const& player : players)
            {
                if (!player.bot)
                    continue;
                if (!first)
                    body += ',';
                first = false;
                body += BuildPlayerJson(player, false);
            }
            body += "]}";
            return body;
        }

        bool ParseResourceId(std::string const& path, std::string_view prefix, uint64_t& id)
        {
            if (path.rfind(prefix, 0) != 0)
                return false;

            std::string const value = path.substr(prefix.size());
            return ParseUnsigned(value, id);
        }

        bool ParsePlayerActionGuid(std::string const& path, std::string_view action, uint32_t& guid)
        {
            std::string const prefix = "/api/v1/players/";
            if (path.rfind(prefix, 0) != 0 || path.size() <= prefix.size() + action.size() ||
                path.compare(path.size() - action.size(), action.size(), action) != 0)
                return false;

            std::string const value = path.substr(prefix.size(), path.size() - prefix.size() - action.size());
            return ParseUnsigned(value, guid);
        }

        bool ParseFloatQuery(std::string const& target, std::string_view key, float& value)
        {
            std::string const raw = QueryValue(target, key);
            if (raw.empty())
                return false;

            try
            {
                std::size_t parsed = 0;
                value = std::stof(raw, &parsed);
                return parsed == raw.size() && std::isfinite(value);
            }
            catch (std::exception const&)
            {
                return false;
            }
        }

        std::string BuildGroupJson(GroupSnapshot const& group)
        {
            std::string body = "{\"id\":" + std::to_string(group.id)
                + ",\"leaderGuid\":" + std::to_string(group.leaderGuid)
                + ",\"raid\":" + (group.raid ? "true" : "false")
                + ",\"members\":[";
            for (std::size_t index = 0; index < group.memberGuids.size(); ++index)
            {
                if (index != 0)
                    body += ',';
                body += std::to_string(group.memberGuids[index]);
            }
            body += "]}";
            return body;
        }

        std::string BuildGroupsResponse()
        {
            std::string body = R"({"data":[)";
            std::vector<GroupSnapshot> const groups = GetGroupSnapshots();
            for (std::size_t index = 0; index < groups.size(); ++index)
            {
                if (index != 0)
                    body += ',';
                body += BuildGroupJson(groups[index]);
            }
            body += "]}";
            return body;
        }

        std::string BuildInstanceJson(InstanceSnapshot const& instance)
        {
            return "{\"instanceId\":" + std::to_string(instance.instanceId)
                + ",\"mapId\":" + std::to_string(instance.mapId)
                + ",\"difficulty\":" + std::to_string(instance.difficulty)
                + ",\"players\":" + std::to_string(instance.players) + "}";
        }

        std::string BuildInstancesResponse()
        {
            std::string body = R"({"data":[)";
            std::vector<InstanceSnapshot> const instances = GetInstanceSnapshots();
            for (std::size_t index = 0; index < instances.size(); ++index)
            {
                if (index != 0)
                    body += ',';
                body += BuildInstanceJson(instances[index]);
            }
            body += "]}";
            return body;
        }

        using WebSocketRequest = boost::beast::http::request<boost::beast::http::empty_body>;

        class WebSocketSession : public std::enable_shared_from_this<WebSocketSession>
        {
        public:
            WebSocketSession(boost::asio::ip::tcp::socket socket, Config config,
                std::shared_ptr<std::atomic_uint32_t> webSocketClients)
                : _socket(std::move(socket)), _config(std::move(config)),
                  _webSocketClients(std::move(webSocketClients)) { }

            ~WebSocketSession()
            {
                ClearSubscriptions();
                if (_webSocketClients)
                    --*_webSocketClients;
            }

            void Start(WebSocketRequest request)
            {
                _socket.set_option(
                    boost::beast::websocket::stream_base::timeout::suggested(boost::beast::role_type::server));
                _socket.read_message_max(_config.maxWebSocketFrameBytes);
                _socket.set_option(boost::beast::websocket::stream_base::decorator(
                    [](boost::beast::websocket::response_type& response)
                {
                    response.set(boost::beast::http::field::server, "mod-server-api");
                }));

                auto self = shared_from_this();
                _socket.async_accept(request, [self](boost::system::error_code error)
                {
                    if (!error)
                        self->ReadMessage();
                });
            }

        private:
            static std::vector<std::string> ParseSubscriptionPatterns(std::string const& message)
            {
                std::vector<std::string> patterns;
                std::size_t const eventsKey = message.find("\"events\"");
                std::size_t const begin = message.find('[', eventsKey);
                std::size_t const end = message.find(']', begin);
                if (eventsKey == std::string::npos || begin == std::string::npos || end == std::string::npos)
                    return patterns;

                for (std::size_t cursor = begin + 1; cursor < end;)
                {
                    std::size_t const quoteStart = message.find('"', cursor);
                    if (quoteStart == std::string::npos || quoteStart >= end)
                        break;
                    std::size_t const quoteEnd = message.find('"', quoteStart + 1);
                    if (quoteEnd == std::string::npos || quoteEnd > end)
                        break;
                    patterns.push_back(message.substr(quoteStart + 1, quoteEnd - quoteStart - 1));
                    cursor = quoteEnd + 1;
                }
                return patterns;
            }

            void ReadMessage()
            {
                auto self = shared_from_this();
                _socket.async_read(_readBuffer, [self](boost::system::error_code error, std::size_t)
                {
                    if (error)
                        return;

                    std::string const message = boost::beast::buffers_to_string(self->_readBuffer.data());
                    self->_readBuffer.consume(self->_readBuffer.size());
                    self->HandleMessage(message);
                    self->ReadMessage();
                });
            }

            void HandleMessage(std::string const& message)
            {
                if (message.find(R"("type":"ping")") != std::string::npos)
                {
                    Enqueue(R"({"type":"pong"})");
                    return;
                }

                if (message.find(R"("type":"unsubscribe")") != std::string::npos)
                {
                    ClearSubscriptions();
                    Enqueue(R"({"type":"unsubscribed"})");
                    return;
                }

                if (message.find(R"("type":"subscribe")") == std::string::npos)
                {
                    Enqueue(R"({"error":{"code":"INVALID_MESSAGE"}})");
                    return;
                }

                std::vector<std::string> patterns = ParseSubscriptionPatterns(message);
                if (patterns.empty() || patterns.size() > _config.maxWebSocketSubscriptions)
                {
                    Enqueue(R"({"error":{"code":"INVALID_SUBSCRIPTIONS"}})");
                    return;
                }

                ClearSubscriptions();
                std::string response = R"({"type":"subscribed","events":[)";
                for (std::string const& pattern : patterns)
                {
                    if (response.back() != '[')
                        response += ',';
                    response += '"' + EscapeJson(pattern) + '"';
                    std::weak_ptr<WebSocketSession> weakSelf = shared_from_this();
                    _subscriptions.push_back(GetEventBus().Subscribe(pattern, [weakSelf](ApiEvent const& event)
                    {
                        if (auto self = weakSelf.lock())
                        {
                            boost::asio::post(self->_socket.get_executor(), [self, event]
                            {
                                self->Enqueue(self->SerializeEvent(event));
                            });
                        }
                    }));
                }
                response += "]}";
                Enqueue(response);
            }

            std::string SerializeEvent(ApiEvent const& event) const
            {
                std::string body = "{\"type\":\"" + EscapeJson(event.type)
                    + "\",\"timestamp\":" + std::to_string(event.timestampMilliseconds)
                    + ",\"version\":1,\"data\":{";
                bool first = true;
                for (auto const& [key, value] : event.data)
                {
                    if (!first)
                        body += ',';
                    first = false;
                    body += Acore::StringFormat("\"{}\":\"{}\"", EscapeJson(key), EscapeJson(value));
                }
                body += "}}";
                return body;
            }

            void Enqueue(std::string message)
            {
                if (message.size() > _config.maxWebSocketFrameBytes || _writeQueue.size() >= _config.maxWebSocketQueue)
                {
                    Close();
                    return;
                }

                bool const wasEmpty = _writeQueue.empty();
                _writeQueue.push_back(std::move(message));
                if (wasEmpty)
                    WriteNext();
            }

            void WriteNext()
            {
                if (_writeQueue.empty())
                    return;

                _socket.text(true);
                auto self = shared_from_this();
                _socket.async_write(boost::asio::buffer(_writeQueue.front()),
                    [self](boost::system::error_code error, std::size_t)
                {
                    if (error)
                        return;
                    self->_writeQueue.pop_front();
                    self->WriteNext();
                });
            }

            void ClearSubscriptions()
            {
                for (SubscriptionId subscription : _subscriptions)
                    GetEventBus().Unsubscribe(subscription);
                _subscriptions.clear();
            }

            void Close()
            {
                ClearSubscriptions();
                boost::system::error_code ignored;
                _socket.close(boost::beast::websocket::close_code::going_away, ignored);
            }

            boost::beast::websocket::stream<boost::asio::ip::tcp::socket> _socket;
            Config _config;
            boost::beast::flat_buffer _readBuffer;
            std::vector<SubscriptionId> _subscriptions;
            std::deque<std::string> _writeQueue;
            std::shared_ptr<std::atomic_uint32_t> _webSocketClients;
        };
    }

    class ApiServer::HttpSession : public std::enable_shared_from_this<ApiServer::HttpSession>
    {
    public:
        HttpSession(boost::asio::ip::tcp::socket socket, Config config,
            std::shared_ptr<std::atomic_uint32_t> webSocketClients,
            std::shared_ptr<RequestRateLimiter> requestRateLimiter)
            : _socket(std::move(socket)), _config(std::move(config)),
              _webSocketClients(std::move(webSocketClients)), _requestRateLimiter(std::move(requestRateLimiter)) { }

        void Start()
        {
            ReadRequest();
        }

    private:
        void ReadRequest()
        {
            auto self = shared_from_this();
            _socket.async_read_some(boost::asio::buffer(_readBuffer),
                [self](boost::system::error_code error, std::size_t bytesRead)
            {
                if (error)
                    return;

                self->_request.append(self->_readBuffer.data(), bytesRead);
                if (self->_request.size() > self->_config.maxRequestBytes)
                {
                    self->WriteResponse(413, "Payload Too Large", R"({"error":{"code":"REQUEST_TOO_LARGE"}})");
                    return;
                }

                if (self->_request.find("\r\n\r\n") == std::string::npos)
                {
                    self->ReadRequest();
                    return;
                }

                self->HandleRequest();
            });
        }

        void HandleRequest()
        {
            if (!_requestRateLimiter->Allow(_config.maxRequestsPerSecond))
            {
                WriteResponse(429, "Too Many Requests", R"({"error":{"code":"TOO_MANY_REQUESTS"}})");
                return;
            }

            std::istringstream requestStream(_request);
            std::string method;
            std::string target;
            std::string version;
            requestStream >> method >> target >> version;

            if ((method != "GET" && method != "POST") || version != "HTTP/1.1")
            {
                WriteResponse(400, "Bad Request", R"({"error":{"code":"BAD_REQUEST"}})");
                return;
            }

            std::string const path = PathFromTarget(target);
            bool const protectedEndpoint = path.starts_with("/api/v1/") || path == "/ws/v1/events";
            if (protectedEndpoint && !HasValidBearerToken(_request, _config))
            {
                WriteResponse(401, "Unauthorized", R"({"error":{"code":"UNAUTHORIZED"}})",
                    "WWW-Authenticate: Bearer\r\n");
                return;
            }

            if (path == "/health")
            {
                if (!RequireMethod(method, "GET"))
                    return;
                WriteResponse(200, "OK", R"({"status":"ok"})");
            }
            else if (path == "/ready")
            {
                if (!RequireMethod(method, "GET"))
                    return;
                WriteResponse(200, "OK", R"({"status":"ready"})");
            }
            else if (path == "/ws/v1/events")
            {
                if (!RequireMethod(method, "GET"))
                    return;
                if (!_config.webSocketEnabled)
                {
                    WriteResponse(404, "Not Found", R"({"error":{"code":"NOT_FOUND"}})");
                    return;
                }

                uint32_t clients = _webSocketClients->load();
                while (clients < _config.maxWebSocketClients &&
                    !_webSocketClients->compare_exchange_weak(clients, clients + 1))
                {
                }
                if (clients >= _config.maxWebSocketClients)
                {
                    WriteResponse(503, "Service Unavailable", R"({"error":{"code":"WS_CLIENT_LIMIT"}})");
                    return;
                }

                auto session = std::make_shared<WebSocketSession>(std::move(_socket), _config, _webSocketClients);
                session->Start(BuildWebSocketRequest(target));
            }
            else if (path == "/api/v1/server" || path == "/api/v1/server/metrics")
            {
                if (!RequireMethod(method, "GET"))
                    return;

                if (path == "/api/v1/server")
                    WriteResponse(200, "OK", BuildServerResponse());
                else
                    WriteResponse(200, "OK", BuildMetricsResponse());
            }
            else if (path == "/api/v1/modules")
            {
                if (!RequireMethod(method, "GET"))
                    return;
                WriteResponse(200, "OK", BuildModulesResponse());
            }
            else if (path == "/api/v1/groups" || path.rfind("/api/v1/groups/", 0) == 0)
            {
                if (!RequireMethod(method, "GET"))
                    return;

                if (path == "/api/v1/groups")
                {
                    WriteResponse(200, "OK", BuildGroupsResponse());
                    return;
                }

                uint64_t id = 0;
                if (!ParseResourceId(path, "/api/v1/groups/", id))
                {
                    WriteResponse(400, "Bad Request", R"({"error":{"code":"INVALID_GROUP_ID"}})");
                    return;
                }
                std::vector<GroupSnapshot> const groups = GetGroupSnapshots();
                auto const group = std::find_if(groups.begin(), groups.end(), [id](GroupSnapshot const& item)
                {
                    return item.id == id;
                });
                if (group == groups.end())
                {
                    WriteResponse(404, "Not Found", R"({"error":{"code":"GROUP_NOT_FOUND"}})");
                    return;
                }
                WriteResponse(200, "OK", BuildGroupJson(*group));
            }
            else if (path == "/api/v1/instances" || path.rfind("/api/v1/instances/", 0) == 0)
            {
                if (!RequireMethod(method, "GET"))
                    return;

                if (path == "/api/v1/instances")
                {
                    WriteResponse(200, "OK", BuildInstancesResponse());
                    return;
                }

                uint64_t id = 0;
                if (!ParseResourceId(path, "/api/v1/instances/", id))
                {
                    WriteResponse(400, "Bad Request", R"({"error":{"code":"INVALID_INSTANCE_ID"}})");
                    return;
                }
                std::vector<InstanceSnapshot> const instances = GetInstanceSnapshots();
                auto const instance = std::find_if(instances.begin(), instances.end(),
                    [id](InstanceSnapshot const& item)
                {
                    return item.instanceId == id;
                });
                if (instance == instances.end())
                {
                    WriteResponse(404, "Not Found", R"({"error":{"code":"INSTANCE_NOT_FOUND"}})");
                    return;
                }
                WriteResponse(200, "OK", BuildInstanceJson(*instance));
            }
            else if (path == "/api/v1/bots")
            {
                if (!RequireMethod(method, "GET"))
                    return;

#ifdef MOD_PLAYERBOTS
                WriteResponse(200, "OK", BuildBotsResponse());
#else
                WriteResponse(501, "Not Implemented", R"({"error":{"code":"NOT_SUPPORTED","feature":"playerbots"}})");
#endif
            }
            else if (method == "POST" && path.rfind("/api/v1/players/", 0) == 0 && path.ends_with("/teleport"))
            {
                uint32_t guid = 0;
                uint32_t mapId = 0;
                float x = 0.0f;
                float y = 0.0f;
                float z = 0.0f;
                float orientation = 0.0f;
                if (!ParsePlayerActionGuid(path, "/teleport", guid) ||
                    !ParseUnsigned(QueryValue(target, "mapId"), mapId) ||
                    !ParseFloatQuery(target, "x", x) || !ParseFloatQuery(target, "y", y) ||
                    !ParseFloatQuery(target, "z", z) || !ParseFloatQuery(target, "orientation", orientation))
                {
                    WriteResponse(400, "Bad Request", R"({"error":{"code":"INVALID_TELEPORT"}})");
                    return;
                }

                if (!EnqueueCommand([guid, mapId, x, y, z, orientation]
                {
                    if (Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guid)))
                        player->TeleportTo(mapId, x, y, z, orientation);
                }))
                    return;

                WriteResponse(202, "Accepted", Acore::StringFormat(
                    R"({{"status":"queued","operation":"player.teleport","guid":{},"mapId":{},)"
                    R"("x":{},"y":{},"z":{},"orientation":{}}})",
                    guid, mapId, x, y, z, orientation));
            }
            else if (method == "POST" && path.rfind("/api/v1/players/", 0) == 0 && path.ends_with("/kick"))
            {
                uint32_t guid = 0;
                if (!ParsePlayerActionGuid(path, "/kick", guid))
                {
                    WriteResponse(400, "Bad Request", R"({"error":{"code":"INVALID_GUID"}})");
                    return;
                }

                if (!EnqueueCommand([guid]
                {
                    if (Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
                        player && player->GetSession())
                        player->GetSession()->KickPlayer("Server API", false);
                }))
                    return;

                WriteResponse(202, "Accepted", Acore::StringFormat(
                    R"({{"status":"queued","operation":"player.kick","guid":{}}})", guid));
            }
            else if (method == "GET" && (path == "/api/v1/players" || path.rfind("/api/v1/players/", 0) == 0))
            {
                if (path == "/api/v1/players")
                {
                    std::string const body = BuildPlayersResponse(target);
                    if (body.find(R"({"error":)") == 0)
                        WriteResponse(400, "Bad Request", body);
                    else
                        WriteResponse(200, "OK", body);
                    return;
                }

                uint64_t guid = 0;
                if (!ParseResourceId(path, "/api/v1/players/", guid))
                {
                    WriteResponse(400, "Bad Request", R"({"error":{"code":"INVALID_GUID"}})");
                    return;
                }

                std::vector<PlayerSnapshot> const players = GetPlayerSnapshots();
                auto const player = std::find_if(players.begin(), players.end(), [guid](PlayerSnapshot const& item)
                {
                    return item.guid == guid;
                });
                if (player == players.end())
                {
                    WriteResponse(404, "Not Found", R"({"error":{"code":"PLAYER_NOT_FOUND"}})");
                    return;
                }
                WriteResponse(200, "OK", BuildPlayerJson(*player, true));
            }
            else
            {
                std::optional<ModuleApiResponse> const response = GetModuleRegistry().Dispatch({
                    method, target, path});
                if (response)
                {
                    WriteResponse(response->status, response->reason, response->body, response->headers);
                    return;
                }
                WriteResponse(404, "Not Found", R"({"error":{"code":"NOT_FOUND"}})");
            }
        }

        bool RequireMethod(std::string_view actual, std::string_view expected)
        {
            if (actual == expected)
                return true;

            WriteResponse(405, "Method Not Allowed", R"({"error":{"code":"METHOD_NOT_ALLOWED"}})",
                Acore::StringFormat("Allow: {}\r\n", expected));
            return false;
        }

        bool EnqueueCommand(std::function<void()> command)
        {
            if (GetCommandQueue().Enqueue(std::move(command)))
                return true;

            WriteResponse(503, "Service Unavailable", R"({"error":{"code":"COMMAND_QUEUE_FULL"}})");
            return false;
        }

        WebSocketRequest BuildWebSocketRequest(std::string const& target) const
        {
            WebSocketRequest request{boost::beast::http::verb::get, target, 11};
            std::size_t lineStart = _request.find("\r\n");
            while (lineStart != std::string::npos)
            {
                lineStart += 2;
                std::size_t lineEnd = _request.find("\r\n", lineStart);
                if (lineEnd == std::string::npos || lineEnd == lineStart)
                    break;

                std::size_t const colon = _request.find(':', lineStart);
                if (colon != std::string::npos && colon < lineEnd)
                {
                    std::string name = _request.substr(lineStart, colon - lineStart);
                    std::string value = _request.substr(colon + 1, lineEnd - colon - 1);
                    while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())))
                        value.erase(value.begin());
                    request.set(name, value);
                }
                lineStart = lineEnd;
            }
            return request;
        }

        void WriteResponse(uint16_t status, std::string_view reason, std::string_view body,
            std::string_view headers = {})
        {
            _response = BuildResponse(status, reason, body, headers);
            auto self = shared_from_this();
            boost::asio::async_write(_socket, boost::asio::buffer(_response),
                [self](boost::system::error_code, std::size_t)
            {
                boost::system::error_code ignored;
                self->_socket.shutdown(boost::asio::ip::tcp::socket::shutdown_both, ignored);
                self->_socket.close(ignored);
            });
        }

        boost::asio::ip::tcp::socket _socket;
        Config _config;
        std::array<char, 4096> _readBuffer{};
        std::string _request;
        std::string _response;
        std::shared_ptr<std::atomic_uint32_t> _webSocketClients;
        std::shared_ptr<RequestRateLimiter> _requestRateLimiter;
    };

    ApiServer::~ApiServer()
    {
        Stop();
    }

    bool ApiServer::Start(Config config)
    {
        if (_running.exchange(true))
            return true;

        _config = std::move(config);
        _ioContext.restart();

        boost::system::error_code error;
        boost::asio::ip::address const address = boost::asio::ip::make_address(_config.bindAddress, error);
        if (error)
        {
            LOG_ERROR("server-api.http", "Invalid bind address {}: {}", _config.bindAddress, error.message());
            _running = false;
            return false;
        }

        _acceptor.open(address.is_v6() ? boost::asio::ip::tcp::v6() : boost::asio::ip::tcp::v4(), error);
        if (!error)
            _acceptor.set_option(boost::asio::socket_base::reuse_address(true), error);
        if (!error)
            _acceptor.bind(boost::asio::ip::tcp::endpoint(address, _config.port), error);
        if (!error)
            _acceptor.listen(boost::asio::socket_base::max_listen_connections, error);

        if (error)
        {
            LOG_ERROR("server-api.http", "Could not bind {}:{}: {}", _config.bindAddress, _config.port,
                error.message());
            _acceptor.close();
            _running = false;
            return false;
        }

        AcceptNext();
        _thread = std::thread([this]
        {
            _ioContext.run();
        });

        LOG_INFO("server-api.http", "Server API listening on {}:{}", _config.bindAddress, _config.port);
        return true;
    }

    void ApiServer::Stop()
    {
        if (!_running.exchange(false))
            return;

        _ioContext.stop();
        if (_thread.joinable())
            _thread.join();

        boost::system::error_code error;
        _acceptor.close(error);
        _ioContext.restart();
        LOG_INFO("server-api.http", "Server API stopped");
    }

    void ApiServer::AcceptNext()
    {
        _acceptor.async_accept([this](boost::system::error_code error, boost::asio::ip::tcp::socket socket)
        {
            if (!error)
                std::make_shared<HttpSession>(std::move(socket), _config, _webSocketClients,
                    _requestRateLimiter)->Start();

            if (_running)
                AcceptNext();
        });
    }
}
