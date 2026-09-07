/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/RequestRateLimiter.h"

namespace ServerApi
{
    bool RequestRateLimiter::Allow(uint32_t maxRequestsPerSecond)
    {
        if (maxRequestsPerSecond == 0)
            return true;

        auto const now = std::chrono::steady_clock::now();
        std::lock_guard lock(_mutex);
        if (_windowStart == std::chrono::steady_clock::time_point{} ||
            now - _windowStart >= std::chrono::seconds(1))
        {
            _windowStart = now;
            _requests = 0;
        }

        if (_requests >= maxRequestsPerSecond)
            return false;

        ++_requests;
        return true;
    }
}
