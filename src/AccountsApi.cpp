/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/AccountsApi.h"

#include "ServerApi/CommandQueue.h"
#include "ServerApi/HttpUtils.h"

#include "AccountMgr.h"
#include "DatabaseEnv.h"
#include "RBAC.h"
#include "WorldSession.h"
#include "WorldSessionMgr.h"

#include "Log.h"
#include "StringFormat.h"
#include "Util.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <string_view>

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

namespace ServerApi
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        constexpr auto AccountOperationTimeout = std::chrono::seconds(2);

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
            std::string_view message, uint32_t accountId, std::string_view stepId, std::string_view stepStatus,
            std::string_view stepMessage)
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

        bool RealmExists(int32_t realmId)
        {
            if (realmId == -1)
                return true;
            QueryResult result = LoginDatabase.Query(Acore::StringFormat(
                "SELECT id FROM realmlist WHERE id = {}", realmId));
            return static_cast<bool>(result);
        }

        using Completion = AccountsApi::Completion;

        ApiResponse CreateError(AccountOpResult result)
        {
            if (result == AOR_NAME_ALREADY_EXIST)
                return Operation(409, "Conflict", "rejected", "Account already exists.", 0,
                    "create", "failed", "Account already exists.");
            if (result == AOR_NAME_TOO_LONG || result == AOR_PASS_TOO_LONG || result == AOR_EMAIL_TOO_LONG)
                return Error(400, "Bad Request", "INVALID_ACCOUNT");
            return Error(500, "Internal Server Error", "ACCOUNT_CREATE_FAILED");
        }

        void CheckAccountAccess(uint32_t accountId, uint8_t securityLevel, int32_t realmId,
            Completion completion, Clock::time_point deadline, std::string message, bool accountCreated = false)
        {
            if (AccountMgr::GetSecurity(accountId, realmId) == securityLevel)
            {
                if (WorldSession* session = sWorldSessionMgr->FindSession(accountId))
                    if (rbac::RBACData* rbac = session->GetRBACData())
                        rbac->SetSecurityLevel(securityLevel);

                if (accountCreated)
                    completion(AccountCreatedWithAccess(201, "Created", "completed", message, accountId,
                        "completed", "Access assigned."));
                else
                    completion(Operation(200, "OK", "completed", message, accountId,
                        "access", "completed", "Access assigned."));
                return;
            }

            if (Clock::now() >= deadline)
            {
                if (accountCreated)
                    completion(AccountCreatedWithAccess(202, "Accepted", "partial",
                        "Account created, but access update is still pending.", accountId,
                        "pending", "Worldserver has not confirmed the access update yet."));
                else
                    completion(Operation(202, "Accepted", "partial", "Access update is still pending.", accountId,
                        "access", "pending", "Worldserver has not confirmed the access update yet."));
                return;
            }

            Completion retryCompletion = completion;
            if (!GetCommandQueue().Enqueue([accountId, securityLevel, realmId,
                completion = std::move(retryCompletion), deadline, message = std::move(message), accountCreated]() mutable
                {
                    CheckAccountAccess(accountId, securityLevel, realmId,
                        std::move(completion), deadline, std::move(message), accountCreated);
                }))
                completion(Error(503, "Service Unavailable", "COMMAND_QUEUE_FULL"));
        }

        void CheckCreatedAccount(CreateRequest request, Completion completion, Clock::time_point deadline)
        {
            uint32_t const accountId = AccountMgr::GetId(request.username);
            if (!accountId)
            {
                if (Clock::now() >= deadline)
                {
                    completion(Operation(202, "Accepted", "partial", "Account creation is still pending.", 0,
                        "create", "pending", "Account has not appeared in the Auth database yet."));
                    return;
                }

                Completion retryCompletion = completion;
                if (!GetCommandQueue().Enqueue([request = std::move(request), completion = std::move(retryCompletion), deadline]() mutable
                    {
                        CheckCreatedAccount(std::move(request), std::move(completion), deadline);
                    }))
                    completion(Error(503, "Service Unavailable", "COMMAND_QUEUE_FULL"));
                return;
            }

            if (request.securityLevel == 0)
            {
                completion(Operation(201, "Created", "completed", "Account created.", accountId,
                    "create", "completed", "Account created."));
                return;
            }

            sAccountMgr->UpdateAccountAccess(nullptr, accountId, request.securityLevel, request.realmId);
            CheckAccountAccess(accountId, request.securityLevel, request.realmId,
                std::move(completion), deadline, "Account created and access assigned.", true);
        }

    }

    bool AccountsApi::Create(std::string body, Completion completion)
    {
        CreateRequest request;
        if (!ParseCreateRequest(body, request))
        {
            completion(Error(400, "Bad Request", "INVALID_ACCOUNT_REQUEST"));
            return true;
        }

        if (!GetCommandQueue().Enqueue([request = std::move(request), completion = std::move(completion)]() mutable
            {
                if (!RealmExists(request.realmId))
                {
                    completion(Error(400, "Bad Request", "REALM_NOT_FOUND"));
                    return;
                }

                AccountOpResult const createResult = sAccountMgr->CreateAccount(
                    request.username, request.password, request.email);
                if (createResult != AOR_OK)
                {
                    completion(CreateError(createResult));
                    return;
                }

                request.password.clear();
                request.email.clear();
                CheckCreatedAccount(std::move(request), std::move(completion), Clock::now() + AccountOperationTimeout);
            }))
            return false;
        return true;
    }

    bool AccountsApi::SetSecurity(uint32_t accountId, std::string body, Completion completion)
    {
        SecurityRequest request;
        if (!accountId || !ParseSecurityRequest(body, request))
        {
            completion(Error(400, "Bad Request", "INVALID_ACCOUNT_SECURITY_REQUEST"));
            return true;
        }

        if (!GetCommandQueue().Enqueue([accountId, request, completion = std::move(completion)]() mutable
            {
                if (!RealmExists(request.realmId))
                {
                    completion(Error(400, "Bad Request", "REALM_NOT_FOUND"));
                    return;
                }
                std::string username;
                if (!AccountMgr::GetName(accountId, username))
                {
                    completion(Operation(404, "Not Found", "rejected", "Account not found.", accountId,
                        "access", "failed", "Account not found."));
                    return;
                }
                sAccountMgr->UpdateAccountAccess(nullptr, accountId, request.securityLevel, request.realmId);
                CheckAccountAccess(accountId, request.securityLevel, request.realmId,
                    std::move(completion), Clock::now() + AccountOperationTimeout, "Account access updated.");
            }))
            return false;
        return true;
    }
}
