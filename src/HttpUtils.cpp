/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/HttpUtils.h"

#include <cctype>

namespace ServerApi
{
    namespace
    {
        bool EqualsIgnoreCase(std::string_view left, std::string_view right)
        {
            if (left.size() != right.size())
                return false;

            for (std::size_t index = 0; index < left.size(); ++index)
            {
                unsigned char const leftCharacter = static_cast<unsigned char>(left[index]);
                unsigned char const rightCharacter = static_cast<unsigned char>(right[index]);
                if (std::tolower(leftCharacter) != std::tolower(rightCharacter))
                    return false;
            }
            return true;
        }
    }

    std::string EscapeJson(std::string_view value)
    {
        static constexpr char HexDigits[] = "0123456789abcdef";

        std::string escaped;
        escaped.reserve(value.size());
        for (unsigned char character : value)
        {
            switch (character)
            {
                case '"':
                    escaped += R"(\")";
                    break;
                case '\\':
                    escaped += R"(\\)";
                    break;
                case '\b':
                    escaped += R"(\b)";
                    break;
                case '\f':
                    escaped += R"(\f)";
                    break;
                case '\n':
                    escaped += R"(\n)";
                    break;
                case '\r':
                    escaped += R"(\r)";
                    break;
                case '\t':
                    escaped += R"(\t)";
                    break;
                default:
                    if (character < 0x20)
                    {
                        escaped += R"(\u00)";
                        escaped.push_back(HexDigits[character >> 4]);
                        escaped.push_back(HexDigits[character & 0x0F]);
                    }
                    else
                        escaped.push_back(static_cast<char>(character));
                    break;
            }
        }
        return escaped;
    }

    std::string HeaderValue(std::string_view request, std::string_view name)
    {
        std::size_t lineStart = request.find("\r\n");
        if (lineStart == std::string_view::npos)
            return {};

        while (lineStart != std::string_view::npos)
        {
            lineStart += 2;
            std::size_t const lineEnd = request.find("\r\n", lineStart);
            if (lineEnd == std::string_view::npos || lineEnd == lineStart)
                break;

            std::size_t const colon = request.find(':', lineStart);
            if (colon != std::string_view::npos && colon < lineEnd &&
                EqualsIgnoreCase(request.substr(lineStart, colon - lineStart), name))
            {
                std::size_t valueStart = colon + 1;
                while (valueStart < lineEnd && (request[valueStart] == ' ' || request[valueStart] == '\t'))
                    ++valueStart;

                std::size_t valueEnd = lineEnd;
                while (valueEnd > valueStart && (request[valueEnd - 1] == ' ' || request[valueEnd - 1] == '\t'))
                    --valueEnd;
                return std::string(request.substr(valueStart, valueEnd - valueStart));
            }

            lineStart = lineEnd;
        }
        return {};
    }

    std::string PathFromTarget(std::string_view target)
    {
        return std::string(target.substr(0, target.find('?')));
    }

    std::string QueryValue(std::string_view target, std::string_view key)
    {
        std::size_t const queryStart = target.find('?');
        if (queryStart == std::string_view::npos)
            return {};

        std::string_view const query = target.substr(queryStart + 1);
        std::size_t parameterStart = 0;
        while (parameterStart < query.size())
        {
            std::size_t parameterEnd = query.find('&', parameterStart);
            if (parameterEnd == std::string_view::npos)
                parameterEnd = query.size();

            std::size_t const equals = query.find('=', parameterStart);
            if (equals < parameterEnd && query.substr(parameterStart, equals - parameterStart) == key)
                return std::string(query.substr(equals + 1, parameterEnd - equals - 1));

            parameterStart = parameterEnd + 1;
        }
        return {};
    }
}
