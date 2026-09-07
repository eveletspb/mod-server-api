/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef SERVER_API_REQUEST_RATE_LIMITER_H_
#define SERVER_API_REQUEST_RATE_LIMITER_H_

#include <chrono>
#include <cstdint>
#include <mutex>

namespace ServerApi
{
    class RequestRateLimiter
    {
    public:
        bool Allow(uint32_t maxRequestsPerSecond);

    private:
        std::mutex _mutex;
        std::chrono::steady_clock::time_point _windowStart;
        uint32_t _requests = 0;
    };
}

#endif
