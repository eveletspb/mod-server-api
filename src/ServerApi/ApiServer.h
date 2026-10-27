/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef SERVER_API_SERVER_H_
#define SERVER_API_SERVER_H_

#include "ServerApi/Authentication.h"
#include "ServerApi/RequestRateLimiter.h"
#include "ServerApi/ServerApiConfig.h"

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/strand.hpp>

namespace ServerApi
{
    class AccountsApi;
    class CharactersApi;
    class WebSocketPublishPool;

    class ApiServer
    {
    public:
        ApiServer() = default;
        ~ApiServer();

        ApiServer(ApiServer const&) = delete;
        ApiServer& operator=(ApiServer const&) = delete;

        bool Start(Config config);
        void Stop();

        [[nodiscard]] bool IsRunning() const { return _running.load(); }

    private:
        class HttpSession;

        void AcceptNext();

        boost::asio::io_context _ioContext;
        boost::asio::ip::tcp::acceptor _acceptor{_ioContext};
        boost::asio::strand<boost::asio::io_context::executor_type> _apiStrand =
            boost::asio::make_strand(_ioContext);
        Config _config;
        std::shared_ptr<AuthenticationProvider> _authenticationProvider;
        bool _authenticationRequired = false;
        std::shared_ptr<AccountsApi> _accountsApi;
        std::shared_ptr<CharactersApi> _charactersApi;
        std::shared_ptr<WebSocketPublishPool> _webSocketPublishPool;
        std::vector<std::thread> _threads;
        std::atomic_bool _running = false;
        std::shared_ptr<std::atomic_uint32_t> _webSocketClients = std::make_shared<std::atomic_uint32_t>(0);
        std::shared_ptr<std::atomic_uint32_t> _sseClients = std::make_shared<std::atomic_uint32_t>(0);
        std::shared_ptr<RequestRateLimiter> _requestRateLimiter = std::make_shared<RequestRateLimiter>();
    };
}

#endif
