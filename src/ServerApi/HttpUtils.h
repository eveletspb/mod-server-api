/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef SERVER_API_HTTP_UTILS_H_
#define SERVER_API_HTTP_UTILS_H_

#include <charconv>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>

namespace ServerApi
{
    std::string EscapeJson(std::string_view value);
    std::string HeaderValue(std::string_view request, std::string_view name);
    std::string PathFromTarget(std::string_view target);
    std::string QueryValue(std::string_view target, std::string_view key);

    template <typename T>
    bool ParseUnsigned(std::string_view value, T& result)
    {
        static_assert(std::is_integral_v<T> && std::is_unsigned_v<T>);

        if (value.empty())
            return false;

        T parsedValue = 0;
        auto const [end, error] = std::from_chars(value.data(), value.data() + value.size(), parsedValue);
        if (error != std::errc{} || end != value.data() + value.size())
            return false;

        result = parsedValue;
        return true;
    }
}

#endif
