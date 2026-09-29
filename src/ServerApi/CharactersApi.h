/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef SERVER_API_CHARACTERS_API_H_
#define SERVER_API_CHARACTERS_API_H_

#include "AsyncCallbackProcessor.h"
#include "DatabaseEnvFwd.h"
#include "QueryCallback.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <boost/asio/io_context.hpp>
#include <boost/asio/steady_timer.hpp>

namespace ServerApi
{
    struct ApiResponse
    {
        uint16_t status = 200;
        std::string reason = "OK";
        std::string body;
    };

    class CharactersApi
    {
    public:
        using Completion = std::function<void(ApiResponse)>;

        explicit CharactersApi(boost::asio::io_context& ioContext);
        ~CharactersApi();

        CharactersApi(CharactersApi const&) = delete;
        CharactersApi& operator=(CharactersApi const&) = delete;

        void Start();
        void Stop();

        // Returns false when the bounded number of in-flight requests is reached.
        bool List(std::string target, Completion completion);
        bool Get(uint32_t guid, Completion completion);

    private:
        struct BotTypeSnapshot;
        struct ListState;
        struct ProfileState;

        bool ReserveRequest();
        void CompleteRequest(Completion completion, ApiResponse response);
        bool SubmitQuery(std::string sql, std::function<void(QueryResult)> callback);
        void QueryOrProbe(std::string sql, std::function<void(QueryResult)> callback,
            std::function<void()> onDatabaseUnavailable);
        void ScheduleCallbackPoll();
        void PollCallbacks();
        void FetchNextListBatch(std::shared_ptr<ListState> state);
        void BeginProfileQueries(std::shared_ptr<ProfileState> state);
        void QueryEquipment(std::shared_ptr<ProfileState> state);
        void QueryProfessions(std::shared_ptr<ProfileState> state);
        std::shared_ptr<BotTypeSnapshot const> GetBotTypes() const;
        void StartBotTypeWorker();
        void StopBotTypeWorker();
        void RefreshBotTypes();

        boost::asio::io_context& _ioContext;
        boost::asio::steady_timer _callbackTimer;
        QueryCallbackProcessor _queryCallbacks;
        uint32_t _activeRequests = 0;
        uint32_t _activeQueries = 0;
        bool _started = false;
        bool _stopped = false;

        mutable std::mutex _botTypesMutex;
        std::shared_ptr<BotTypeSnapshot const> _botTypes;
        std::mutex _botWorkerMutex;
        std::condition_variable _botWorkerCondition;
        bool _stopBotWorker = false;
        std::thread _botWorker;
    };
}

#endif
