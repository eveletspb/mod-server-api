/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/Authentication.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace ServerApi
{
    namespace
    {
        bool IsValidProviderName(std::string_view name)
        {
            return !name.empty() && std::all_of(name.begin(), name.end(), [](char character)
            {
                return (character >= 'a' && character <= 'z') ||
                    (character >= '0' && character <= '9') || character == '-';
            });
        }

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

    std::string_view AuthenticationRequest::HeaderValue(std::string_view name) const
    {
        auto const header = std::find_if(headers.begin(), headers.end(), [name](AuthenticationHeader const& item)
        {
            return EqualsIgnoreCase(item.name, name);
        });
        return header == headers.end() ? std::string_view{} : std::string_view(header->value);
    }

    AuthenticationResult AuthenticationResult::AllowAnonymous()
    {
        return {true, std::nullopt, {}};
    }

    AuthenticationResult AuthenticationResult::Authenticated(std::string subject)
    {
        return {true, std::move(subject), {}};
    }

    AuthenticationResult AuthenticationResult::Reject(std::string challenge)
    {
        return {false, std::nullopt, std::move(challenge)};
    }

    AuthenticationResult NoAuthProvider::Authenticate(AuthenticationRequest const&) const
    {
        return AuthenticationResult::AllowAnonymous();
    }

    bool IsAuthenticationProviderAllowedForBind(bool providerRequiresAuthentication, bool isLoopbackAddress)
    {
        return isLoopbackAddress || providerRequiresAuthentication;
    }

    AuthenticationProviderRegistry::AuthenticationProviderRegistry()
    {
        _providers.emplace_back("none", []
        {
            return std::make_unique<NoAuthProvider>();
        });
    }

    bool AuthenticationProviderRegistry::Register(std::string name, AuthenticationProviderFactory factory)
    {
        if (!IsValidProviderName(name) || !factory)
            return false;

        std::lock_guard<std::mutex> lock(_mutex);
        auto const existing = std::find_if(_providers.begin(), _providers.end(), [&name](auto const& provider)
        {
            return provider.first == name;
        });
        if (existing != _providers.end())
            return false;

        _providers.emplace_back(std::move(name), std::move(factory));
        return true;
    }

    std::unique_ptr<AuthenticationProvider> AuthenticationProviderRegistry::Create(std::string_view name) const
    {
        AuthenticationProviderFactory factory;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto const existing = std::find_if(_providers.begin(), _providers.end(), [name](auto const& provider)
            {
                return provider.first == name;
            });
            if (existing == _providers.end())
                return {};
            factory = existing->second;
        }
        return factory();
    }

    bool AuthenticationProviderRegistry::Contains(std::string_view name) const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return std::any_of(_providers.begin(), _providers.end(), [name](auto const& provider)
        {
            return provider.first == name;
        });
    }

    AuthenticationProviderRegistry& GetAuthenticationProviderRegistry()
    {
        static AuthenticationProviderRegistry registry;
        return registry;
    }
}
