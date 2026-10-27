/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/AccountsApi.h"

#include "ServerApi/CommandQueue.h"
#include "ServerApi/HttpUtils.h"

#include "DatabaseEnv.h"
#include "Field.h"
#include "Log.h"
#include "QueryResult.h"
#include "RBAC.h"
#include "SRP6.h"
#include "StringFormat.h"
#include "Util.h"
#include "World.h"
#include "WorldConfig.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include <algorithm>
#include <chrono>
#include <cctype>
#include <exception>
#include <sstream>
#include <string_view>
#include <utility>

#include <boost/asio/post.hpp>
#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace ServerApi
{
    namespace
    {
        constexpr uint32_t MaxConcurrentRequests = 32;
        constexpr auto CallbackPollInterval = std::chrono::milliseconds(10);

        struct CreateRequest
        {
            std::string username;
            std::string password;
            std::string email;
            uint8_t securityLevel = 0;
            int32_t realmId = 0;
        };

        struct SecurityRequest
        {
            uint8_t securityLevel = 0;
            int32_t realmId = 0;
        };

        ApiResponse Error(uint16_t status, std::string_view reason, std::string_view code)
        {
            return {status, std::string(reason), Acore::StringFormat(
                R"({{"error":{{"code":"{}"}}}})", EscapeJson(code))};
        }

        ApiResponse Operation(uint16_t status, std::string_view reason, std::string_view state,
            std::string_view message, uint32_t accountId, std::string_view stepId,
            std::string_view stepStatus, std::string_view stepMessage)
        {
            return {status, std::string(reason), Acore::StringFormat(
                R"({{"status":"{}","message":"{}","accountId":{},"steps":[{{"id":"{}","status":"{}","message":"{}"}}]}})",
                state, EscapeJson(message), accountId, stepId, stepStatus, EscapeJson(stepMessage))};
        }

        ApiResponse AccountCreatedWithAccess(uint16_t status, std::string_view reason,
            std::string_view state, std::string_view message, uint32_t accountId,
            std::string_view accessStatus, std::string_view accessMessage)
        {
            return {status, std::string(reason), Acore::StringFormat(
                R"({{"status":"{}","message":"{}","accountId":{},"steps":[{{"id":"create","status":"completed","message":"Account created."}},{{"id":"access","status":"{}","message":"{}"}}]}})",
                state, EscapeJson(message), accountId, accessStatus, EscapeJson(accessMessage))};
        }

        bool ParseObject(std::string const& body, boost::property_tree::ptree& object)
        {
            try
            {
                std::istringstream input(body);
                boost::property_tree::read_json(input, object);
                return true;
            }
            catch (boost::property_tree::json_parser::json_parser_error const&)
            {
                return false;
            }
        }

        bool ValidUsername(std::string_view value)
        {
            return !value.empty() && value.size() <= 17 && std::all_of(value.begin(), value.end(),
                [](unsigned char character) { return std::isalnum(character) && character < 0x80; });
        }

        bool ValidPassword(std::string_view value)
        {
            std::string password(value);
            return !password.empty() && utf8length(password) <= 16 && std::all_of(value.begin(), value.end(),
                [](unsigned char character) { return character > 0x20 && character != 0x7f; });
        }

        bool ParseCreateRequest(std::string const& body, CreateRequest& request)
        {
            boost::property_tree::ptree object;
            if (!ParseObject(body, object))
                return false;

            auto username = object.get_optional<std::string>("username");
            auto password = object.get_optional<std::string>("password");
            auto email = object.get_optional<std::string>("email");
            auto securityLevel = object.get_optional<int>("securityLevel");
            auto realmId = object.get_optional<int>("realmId");
            if (!username || !password || !email || !securityLevel || !realmId ||
                !ValidUsername(*username) || !ValidPassword(*password) || email->size() > 255 ||
                *securityLevel < 0 || *securityLevel > 3 || *realmId < -1 || *realmId == 0)
                return false;

            request = {std::move(*username), std::move(*password), std::move(*email),
                static_cast<uint8_t>(*securityLevel), static_cast<int32_t>(*realmId)};
            return true;
        }

        bool ParseSecurityRequest(std::string const& body, SecurityRequest& request)
        {
            boost::property_tree::ptree object;
            if (!ParseObject(body, object))
                return false;

            auto securityLevel = object.get_optional<int>("securityLevel");
            auto realmId = object.get_optional<int>("realmId");
            if (!securityLevel || !realmId || *securityLevel < 0 || *securityLevel > 3 ||
                *realmId < -1 || *realmId == 0)
                return false;

            request = {static_cast<uint8_t>(*securityLevel), static_cast<int32_t>(*realmId)};
            return true;
        }

        std::string AccountLookupQuery(std::string_view username)
        {
            // Usernames are restricted to ASCII letters and digits before this query is built.
            return Acore::StringFormat("SELECT id FROM account WHERE username = '{}'", username);
        }

        std::string AccountIdQuery(uint32_t accountId)
        {
            return Acore::StringFormat("SELECT id FROM account WHERE id = {}", accountId);
        }

        std::string RealmQuery(int32_t realmId)
        {
            return Acore::StringFormat("SELECT id FROM realmlist WHERE id = {}", realmId);
        }

        ApiResponse DatabaseUnavailable()
        {
            return Error(503, "Service Unavailable", "LOGIN_DATABASE_UNAVAILABLE");
        }
    }

    struct AccountsApi::RequestState
    {
        ~RequestState()
        {
            std::fill(password.begin(), password.end(), '\0');
        }

        Completion completion;
        std::string username;
        std::string password;
        std::string email;
        uint32_t accountId = 0;
        uint8_t securityLevel = 0;
        int32_t realmId = 0;
        bool accountCreated = false;
        bool completed = false;
    };

    AccountsApi::AccountsApi(boost::asio::strand<boost::asio::io_context::executor_type> strand)
        : _strand(std::move(strand)), _callbackTimer(_strand) { }

    AccountsApi::~AccountsApi()
    {
        Stop();
    }

    void AccountsApi::Start()
    {
        if (_started)
            return;

        _expansion = static_cast<uint8_t>(sWorld->getIntConfig(CONFIG_EXPANSION));
        _started = true;
        _stopped = false;
        ScheduleCallbackPoll();
    }

    void AccountsApi::Stop()
    {
        if (_stopped)
            return;

        _stopped = true;
        _callbackTimer.cancel();
    }

    bool AccountsApi::ReserveRequest()
    {
        if (!_started || _stopped || _activeRequests >= MaxConcurrentRequests)
            return false;

        ++_activeRequests;
        return true;
    }

    void AccountsApi::CompleteRequest(std::shared_ptr<RequestState> const& state, ApiResponse response)
    {
        if (state->completed)
            return;

        state->completed = true;
        if (_activeRequests > 0)
            --_activeRequests;
        if (state->completion)
            state->completion(std::move(response));
    }

    bool AccountsApi::SubmitQuery(std::string sql, std::function<void(QueryResult)> callback)
    {
        if (_stopped || _activeQueries >= MaxConcurrentRequests)
            return false;

        try
        {
            ++_activeQueries;
            QueryCallback query = LoginDatabase.AsyncQuery(sql).WithCallback(
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
            LOG_ERROR("server-api.accounts", "Could not enqueue login database query: {}", exception.what());
            return false;
        }
    }

    bool AccountsApi::SubmitQueryOrProbe(std::string sql,
        std::function<void(QueryResult, bool)> callback)
    {
        auto sharedCallback = std::make_shared<std::function<void(QueryResult, bool)>>(std::move(callback));
        return SubmitQuery(std::move(sql), [this, sharedCallback](QueryResult result)
        {
            if (result)
            {
                (*sharedCallback)(std::move(result), true);
                return;
            }

            if (!SubmitQuery("SELECT 1", [sharedCallback](QueryResult probe) mutable
                {
                    (*sharedCallback)(nullptr, static_cast<bool>(probe));
                }))
                (*sharedCallback)(nullptr, false);
        });
    }

    bool AccountsApi::SubmitTransaction(LoginDatabaseTransaction transaction,
        std::function<void(bool)> callback)
    {
        if (_stopped)
            return false;

        try
        {
            TransactionCallback transactionCallback = LoginDatabase.AsyncCommitTransaction(std::move(transaction));
            transactionCallback.AfterComplete(std::move(callback));
            _transactionCallbacks.AddCallback(std::move(transactionCallback));
            return true;
        }
        catch (std::exception const& exception)
        {
            LOG_ERROR("server-api.accounts", "Could not enqueue login database transaction: {}", exception.what());
            return false;
        }
    }

    void AccountsApi::ScheduleCallbackPoll()
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

    void AccountsApi::PollCallbacks()
    {
        _queryCallbacks.ProcessReadyCallbacks();
        _transactionCallbacks.ProcessReadyCallbacks();
    }

    bool AccountsApi::Create(std::string body, Completion completion)
    {
        CreateRequest request;
        if (!ParseCreateRequest(body, request))
        {
            completion(Error(400, "Bad Request", "INVALID_ACCOUNT_REQUEST"));
            return true;
        }
        if (!ReserveRequest())
            return false;

        auto state = std::make_shared<RequestState>();
        state->completion = std::move(completion);
        state->username = std::move(request.username);
        state->password = std::move(request.password);
        state->email = std::move(request.email);
        state->securityLevel = request.securityLevel;
        state->realmId = request.realmId;
        ValidateCreateRealm(std::move(state));
        return true;
    }

    void AccountsApi::ValidateCreateRealm(std::shared_ptr<RequestState> state)
    {
        if (state->realmId == -1)
        {
            CheckCreateDuplicate(std::move(state));
            return;
        }

        if (!SubmitQueryOrProbe(RealmQuery(state->realmId), [this, state](QueryResult result, bool databaseAvailable)
            {
                if (!databaseAvailable)
                {
                    CompleteRequest(state, DatabaseUnavailable());
                    return;
                }
                if (!result)
                {
                    CompleteRequest(state, Error(400, "Bad Request", "REALM_NOT_FOUND"));
                    return;
                }
                CheckCreateDuplicate(state);
            }))
            CompleteRequest(state, DatabaseUnavailable());
    }

    void AccountsApi::CheckCreateDuplicate(std::shared_ptr<RequestState> state)
    {
        std::string normalizedUsername = state->username;
        Utf8ToUpperOnlyLatin(normalizedUsername);
        if (!SubmitQueryOrProbe(AccountLookupQuery(normalizedUsername),
            [this, state](QueryResult result, bool databaseAvailable)
            {
                if (!databaseAvailable)
                {
                    CompleteRequest(state, DatabaseUnavailable());
                    return;
                }
                if (result)
                {
                    CompleteRequest(state, Operation(409, "Conflict", "rejected",
                        "Account already exists.", 0, "create", "failed", "Account already exists."));
                    return;
                }
                InsertAccount(state);
            }))
            CompleteRequest(state, DatabaseUnavailable());
    }

    void AccountsApi::ResolveCreateFailure(std::shared_ptr<RequestState> state)
    {
        if (!SubmitQueryOrProbe(AccountLookupQuery(state->username), [this, state](QueryResult result,
            bool databaseAvailable)
            {
                if (!databaseAvailable)
                    CompleteRequest(state, DatabaseUnavailable());
                else if (result)
                    CompleteRequest(state, Operation(409, "Conflict", "rejected",
                        "Account already exists.", 0, "create", "failed", "Account already exists."));
                else
                    CompleteRequest(state, Error(500, "Internal Server Error", "ACCOUNT_CREATE_FAILED"));
            }))
            CompleteRequest(state, DatabaseUnavailable());
    }

    void AccountsApi::InsertAccount(std::shared_ptr<RequestState> state)
    {
        try
        {
            Utf8ToUpperOnlyLatin(state->username);
            Utf8ToUpperOnlyLatin(state->password);
            Utf8ToUpperOnlyLatin(state->email);
            auto [salt, verifier] = Acore::Crypto::SRP6::MakeRegistrationData(state->username, state->password);
            std::fill(state->password.begin(), state->password.end(), '\0');
            state->password.clear();

            LoginDatabaseTransaction transaction = LoginDatabase.BeginTransaction();
            LoginDatabasePreparedStatement* account = LoginDatabase.GetPreparedStatement(LOGIN_INS_ACCOUNT);
            account->SetData(0, state->username);
            account->SetData(1, salt);
            account->SetData(2, verifier);
            account->SetData(3, _expansion);
            account->SetData(4, state->email);
            account->SetData(5, state->email);
            transaction->Append(account);
            transaction->Append(LoginDatabase.GetPreparedStatement(LOGIN_INS_REALM_CHARACTERS_INIT));

            if (!SubmitTransaction(std::move(transaction), [this, state](bool success)
            {
                if (!success)
                {
                    ResolveCreateFailure(state);
                    return;
                }
                state->accountCreated = true;
                FindCreatedAccount(state);
            }))
                CompleteRequest(state, DatabaseUnavailable());
        }
        catch (std::exception const& exception)
        {
            LOG_ERROR("server-api.accounts", "Could not prepare account creation: {}", exception.what());
            CompleteRequest(state, Error(500, "Internal Server Error", "ACCOUNT_CREATE_FAILED"));
        }
    }

    void AccountsApi::FindCreatedAccount(std::shared_ptr<RequestState> state)
    {
        auto idPending = [this, state]
        {
            if (state->securityLevel == 0)
                CompleteRequest(state, Operation(202, "Accepted", "partial",
                    "Account was created, but its ID is not available yet.", 0,
                    "create", "pending", "Account ID lookup is pending."));
            else
                CompleteRequest(state, AccountCreatedWithAccess(202, "Accepted", "partial",
                    "Account was created, but its ID is not available yet.", 0,
                    "pending", "Account ID lookup is pending."));
        };

        if (!SubmitQueryOrProbe(AccountLookupQuery(state->username), [this, state, idPending](QueryResult result,
            bool databaseAvailable) mutable
            {
                if (!databaseAvailable || !result)
                {
                    idPending();
                    return;
                }

                state->accountId = result->Fetch()[0].Get<uint32_t>();
                if (state->securityLevel == 0)
                {
                    CompleteRequest(state, Operation(201, "Created", "completed", "Account created.",
                        state->accountId, "create", "completed", "Account created."));
                    return;
                }
                UpdateAccountSecurity(state);
            }))
            idPending();
    }

    bool AccountsApi::SetSecurity(uint32_t accountId, std::string body, Completion completion)
    {
        SecurityRequest request;
        if (!accountId || !ParseSecurityRequest(body, request))
        {
            completion(Error(400, "Bad Request", "INVALID_ACCOUNT_SECURITY_REQUEST"));
            return true;
        }
        if (!ReserveRequest())
            return false;

        auto state = std::make_shared<RequestState>();
        state->completion = std::move(completion);
        state->accountId = accountId;
        state->securityLevel = request.securityLevel;
        state->realmId = request.realmId;
        ValidateSecurityTarget(std::move(state));
        return true;
    }

    void AccountsApi::ValidateSecurityTarget(std::shared_ptr<RequestState> state)
    {
        auto checkAccount = [this, state]
        {
            if (!SubmitQueryOrProbe(AccountIdQuery(state->accountId),
                [this, state](QueryResult result, bool databaseAvailable)
                {
                    if (!databaseAvailable)
                    {
                        CompleteRequest(state, DatabaseUnavailable());
                        return;
                    }
                    if (!result)
                    {
                        CompleteRequest(state, Operation(404, "Not Found", "rejected",
                            "Account not found.", state->accountId, "access", "failed", "Account not found."));
                        return;
                    }
                    UpdateAccountSecurity(state);
                }))
                CompleteRequest(state, DatabaseUnavailable());
        };

        if (state->realmId == -1)
        {
            checkAccount();
            return;
        }

        if (!SubmitQueryOrProbe(RealmQuery(state->realmId), [this, state, checkAccount](QueryResult result,
            bool databaseAvailable) mutable
            {
                if (!databaseAvailable)
                {
                    CompleteRequest(state, DatabaseUnavailable());
                    return;
                }
                if (!result)
                {
                    CompleteRequest(state, Error(400, "Bad Request", "REALM_NOT_FOUND"));
                    return;
                }
                checkAccount();
            }))
            CompleteRequest(state, DatabaseUnavailable());
    }

    void AccountsApi::UpdateAccountSecurity(std::shared_ptr<RequestState> state)
    {
        try
        {
            LoginDatabaseTransaction transaction = LoginDatabase.BeginTransaction();
            if (state->realmId == -1)
            {
                LoginDatabasePreparedStatement* statement = LoginDatabase.GetPreparedStatement(LOGIN_DEL_ACCOUNT_ACCESS);
                statement->SetData(0, state->accountId);
                transaction->Append(statement);
            }
            else
            {
                LoginDatabasePreparedStatement* statement =
                    LoginDatabase.GetPreparedStatement(LOGIN_DEL_ACCOUNT_ACCESS_BY_REALM);
                statement->SetData(0, state->accountId);
                statement->SetData(1, state->realmId);
                transaction->Append(statement);
            }

            if (state->securityLevel)
            {
                LoginDatabasePreparedStatement* statement = LoginDatabase.GetPreparedStatement(LOGIN_INS_ACCOUNT_ACCESS);
                statement->SetData(0, state->accountId);
                statement->SetData(1, state->securityLevel);
                statement->SetData(2, state->realmId);
                transaction->Append(statement);
            }

            if (!SubmitTransaction(std::move(transaction), [this, state](bool success)
            {
                if (!success)
                {
                    if (state->accountCreated)
                        CompleteRequest(state, AccountCreatedWithAccess(202, "Accepted", "partial",
                            "Account was created, but security assignment failed.", state->accountId,
                            "failed", "The account access transaction failed."));
                    else
                        CompleteRequest(state, Operation(500, "Internal Server Error", "rejected",
                            "Account access update failed.", state->accountId, "access", "failed",
                            "The account access transaction failed."));
                    return;
                }
                ApplyOnlineSecurity(state);
            }))
            {
                if (state->accountCreated)
                    CompleteRequest(state, AccountCreatedWithAccess(202, "Accepted", "partial",
                        "Account was created, but security assignment could not be queued.", state->accountId,
                        "pending", "The account access transaction is pending."));
                else
                    CompleteRequest(state, DatabaseUnavailable());
            }
        }
        catch (std::exception const& exception)
        {
            LOG_ERROR("server-api.accounts", "Could not prepare account access update: {}", exception.what());
            if (state->accountCreated)
                CompleteRequest(state, AccountCreatedWithAccess(202, "Accepted", "partial",
                    "Account was created, but security assignment failed.", state->accountId,
                    "failed", "The account access transaction could not be prepared."));
            else
                CompleteRequest(state, Error(500, "Internal Server Error", "ACCOUNT_ACCESS_UPDATE_FAILED"));
        }
    }

    void AccountsApi::ApplyOnlineSecurity(std::shared_ptr<RequestState> state)
    {
        std::weak_ptr<AccountsApi> weakSelf = shared_from_this();
        if (!GetCommandQueue().Enqueue([weakSelf, state]
            {
                if (WorldSession* session = sWorldSessionMgr->FindSession(state->accountId))
                    if (rbac::RBACData* rbac = session->GetRBACData())
                        rbac->SetSecurityLevel(state->securityLevel);

                if (std::shared_ptr<AccountsApi> self = weakSelf.lock())
                    boost::asio::post(self->_strand, [self, state]
                    {
                        if (state->accountCreated)
                            self->CompleteRequest(state, AccountCreatedWithAccess(201, "Created", "completed",
                                "Account created and access assigned.", state->accountId,
                                "completed", "Access assigned."));
                        else
                            self->CompleteRequest(state, Operation(200, "OK", "completed",
                                "Account access updated.", state->accountId, "access", "completed",
                                "Access assigned."));
                    });
            }))
        {
            if (state->accountCreated)
                CompleteRequest(state, AccountCreatedWithAccess(202, "Accepted", "partial",
                    "Account was created and access was saved; online session refresh is pending.",
                    state->accountId, "pending", "Online session refresh is pending."));
            else
                CompleteRequest(state, Operation(202, "Accepted", "partial",
                    "Account access was saved; online session refresh is pending.",
                    state->accountId, "access", "pending", "Online session refresh is pending."));
        }
    }
}
