/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef SERVER_API_ACCOUNTS_API_H_
#define SERVER_API_ACCOUNTS_API_H_

#include "ServerApi/CharactersApi.h"

#include "AsyncCallbackProcessor.h"
#include "DatabaseEnvFwd.h"
#include "QueryCallback.h"
#include "Transaction.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <boost/asio/io_context.hpp>
#include <boost/asio/strand.hpp>
#include <boost/asio/steady_timer.hpp>

namespace ServerApi
{
    class AccountsApi : public std::enable_shared_from_this<AccountsApi>
    {
    public:
        using Completion = std::function<void(ApiResponse)>;

        explicit AccountsApi(boost::asio::strand<boost::asio::io_context::executor_type> strand);
        ~AccountsApi();

        AccountsApi(AccountsApi const&) = delete;
        AccountsApi& operator=(AccountsApi const&) = delete;

        void Start();
        void Stop();

        bool Create(std::string body, Completion completion);
        bool SetSecurity(uint32_t accountId, std::string body, Completion completion);

    private:
        struct RequestState;

        bool ReserveRequest();
        void CompleteRequest(std::shared_ptr<RequestState> const& state, ApiResponse response);
        bool SubmitQuery(std::string sql, std::function<void(QueryResult)> callback);
        bool SubmitQueryOrProbe(std::string sql,
            std::function<void(QueryResult, bool)> callback);
        bool SubmitTransaction(LoginDatabaseTransaction transaction,
            std::function<void(bool)> callback);
        void ValidateCreateRealm(std::shared_ptr<RequestState> state);
        void CheckCreateDuplicate(std::shared_ptr<RequestState> state);
        void ResolveCreateFailure(std::shared_ptr<RequestState> state);
        void InsertAccount(std::shared_ptr<RequestState> state);
        void FindCreatedAccount(std::shared_ptr<RequestState> state);
        void ValidateSecurityTarget(std::shared_ptr<RequestState> state);
        void UpdateAccountSecurity(std::shared_ptr<RequestState> state);
        void ApplyOnlineSecurity(std::shared_ptr<RequestState> state);
        void ScheduleCallbackPoll();
        void PollCallbacks();

        boost::asio::strand<boost::asio::io_context::executor_type> _strand;
        boost::asio::steady_timer _callbackTimer;
        QueryCallbackProcessor _queryCallbacks;
        AsyncCallbackProcessor<TransactionCallback> _transactionCallbacks;
        uint32_t _activeRequests = 0;
        uint32_t _activeQueries = 0;
        uint8_t _expansion = 0;
        bool _started = false;
        bool _stopped = false;
    };
}

#endif
