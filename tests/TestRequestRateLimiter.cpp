/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "gtest/gtest.h"

#include "ServerApi/RequestRateLimiter.h"

namespace
{
    TEST(ServerApiRequestRateLimiter, AllowsConfiguredNumberOfRequests)
    {
        ServerApi::RequestRateLimiter limiter;

        EXPECT_TRUE(limiter.Allow(2));
        EXPECT_TRUE(limiter.Allow(2));
        EXPECT_FALSE(limiter.Allow(2));
    }

    TEST(ServerApiRequestRateLimiter, ZeroDisablesLimit)
    {
        ServerApi::RequestRateLimiter limiter;

        for (int i = 0; i < 100; ++i)
            EXPECT_TRUE(limiter.Allow(0));
    }
}
