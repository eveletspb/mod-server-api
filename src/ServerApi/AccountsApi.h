/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef SERVER_API_ACCOUNTS_API_H_
#define SERVER_API_ACCOUNTS_API_H_

#include "ServerApi/CharactersApi.h"

#include <cstdint>
#include <functional>
#include <string>

namespace ServerApi
{
    class AccountsApi
    {
    public:
        using Completion = std::function<void(ApiResponse)>;

        static bool Create(std::string body, Completion completion);
        static bool SetSecurity(uint32_t accountId, std::string body, Completion completion);
    };
}

#endif
