/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "gtest/gtest.h"

#include "ServerApi/HttpUtils.h"

#include <cstdint>
#include <limits>
#include <string>

namespace
{
    TEST(ServerApiHttpUtils, EscapesJsonSpecialAndControlCharacters)
    {
        std::string const input = "quote\" slash\\ newline\n tab\t control" + std::string(1, '\x01');

        EXPECT_EQ(ServerApi::EscapeJson(input), R"(quote\" slash\\ newline\n tab\t control\u0001)");
    }

    TEST(ServerApiHttpUtils, ReadsHeaderCaseInsensitivelyAndTrimsWhitespace)
    {
        std::string const request =
            "GET /api/v1/server HTTP/1.1\r\n"
            "Host: localhost\r\n"
            "X-Token:\tsecret  \r\n\r\n";

        EXPECT_EQ(ServerApi::HeaderValue(request, "x-token"), "secret");
        EXPECT_TRUE(ServerApi::HeaderValue(request, "missing").empty());
    }

    TEST(ServerApiHttpUtils, ParsesTargetPathAndExactQueryKeys)
    {
        std::string const target = "/api/v1/players?mapId=1&limit=25&empty=";

        EXPECT_EQ(ServerApi::PathFromTarget(target), "/api/v1/players");
        EXPECT_EQ(ServerApi::QueryValue(target, "mapId"), "1");
        EXPECT_EQ(ServerApi::QueryValue(target, "limit"), "25");
        EXPECT_TRUE(ServerApi::QueryValue(target, "map").empty());
        EXPECT_TRUE(ServerApi::QueryValue(target, "empty").empty());
    }

    TEST(ServerApiHttpUtils, ParsesUnsignedValuesWithoutOverflow)
    {
        uint32_t value = 0;
        uint64_t largeValue = 0;

        EXPECT_TRUE(ServerApi::ParseUnsigned("4294967295", value));
        EXPECT_EQ(value, std::numeric_limits<uint32_t>::max());
        EXPECT_FALSE(ServerApi::ParseUnsigned("4294967296", value));
        EXPECT_TRUE(ServerApi::ParseUnsigned("4294967296", largeValue));
        EXPECT_EQ(largeValue, 4294967296ULL);
        EXPECT_FALSE(ServerApi::ParseUnsigned("-1", value));
        EXPECT_FALSE(ServerApi::ParseUnsigned("1x", value));
    }
}
