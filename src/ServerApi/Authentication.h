/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef SERVER_API_AUTHENTICATION_H_
#define SERVER_API_AUTHENTICATION_H_

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ServerApi
{
    struct AuthenticationHeader
    {
        std::string name;
        std::string value;
    };

    struct AuthenticationRequest
    {
        std::string method;
        std::string target;
        std::vector<AuthenticationHeader> headers;

        [[nodiscard]] std::string_view HeaderValue(std::string_view name) const;
    };

    struct AuthIdentity
    {
        std::string provider;
        std::string subject;
    };

    struct AuthenticationResult
    {
        bool accepted = false;
        std::optional<std::string> subject;
        std::string challenge;

        static AuthenticationResult AllowAnonymous();
        static AuthenticationResult Authenticated(std::string subject);
        static AuthenticationResult Reject(std::string challenge = {});
    };

    class AuthenticationProvider
    {
    public:
        virtual ~AuthenticationProvider() = default;

        [[nodiscard]] virtual bool RequiresAuthentication() const = 0;
        [[nodiscard]] virtual AuthenticationResult Authenticate(
            AuthenticationRequest const& request) const = 0;
    };

    [[nodiscard]] bool IsAuthenticationProviderAllowedForBind(
        bool providerRequiresAuthentication, bool isLoopbackAddress);

    class NoAuthProvider final : public AuthenticationProvider
    {
    public:
        [[nodiscard]] bool RequiresAuthentication() const override { return false; }
        [[nodiscard]] AuthenticationResult Authenticate(AuthenticationRequest const&) const override;
    };

    using AuthenticationProviderFactory = std::function<std::unique_ptr<AuthenticationProvider>()>;

    class AuthenticationProviderRegistry
    {
    public:
        AuthenticationProviderRegistry();

        AuthenticationProviderRegistry(AuthenticationProviderRegistry const&) = delete;
        AuthenticationProviderRegistry& operator=(AuthenticationProviderRegistry const&) = delete;

        bool Register(std::string name, AuthenticationProviderFactory factory);
        [[nodiscard]] std::unique_ptr<AuthenticationProvider> Create(std::string_view name) const;
        [[nodiscard]] bool Contains(std::string_view name) const;

    private:
        mutable std::mutex _mutex;
        std::vector<std::pair<std::string, AuthenticationProviderFactory>> _providers;
    };

    AuthenticationProviderRegistry& GetAuthenticationProviderRegistry();
}

#endif
