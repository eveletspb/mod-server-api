/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "gtest/gtest.h"

#include "ServerApi/Authentication.h"
#include "ServerApi/ServerApiConfig.h"

namespace
{
    class TestAuthenticationProvider final : public ServerApi::AuthenticationProvider
    {
    public:
        [[nodiscard]] bool RequiresAuthentication() const override { return true; }

        [[nodiscard]] ServerApi::AuthenticationResult Authenticate(
            ServerApi::AuthenticationRequest const& request) const override
        {
            if (request.HeaderValue("x-user-id").empty())
                return ServerApi::AuthenticationResult::Reject("Test realm=\"server-api\"");
            return ServerApi::AuthenticationResult::Authenticated(std::string(request.HeaderValue("x-user-id")));
        }
    };

    TEST(ServerApiAuthentication, RegistersNoAuthProviderByDefault)
    {
        EXPECT_EQ(ServerApi::Config{}.authProvider, "none");

        ServerApi::AuthenticationProviderRegistry registry;
        EXPECT_TRUE(registry.Contains("none"));

        std::unique_ptr<ServerApi::AuthenticationProvider> provider = registry.Create("none");
        ASSERT_NE(provider, nullptr);
        EXPECT_FALSE(provider->RequiresAuthentication());
        EXPECT_TRUE(provider->Authenticate({}).accepted);
        EXPECT_FALSE(provider->Authenticate({}).subject.has_value());
    }

    TEST(ServerApiAuthentication, RejectsInvalidAndDuplicateProviderNames)
    {
        ServerApi::AuthenticationProviderRegistry registry;
        auto factory = [] { return std::make_unique<TestAuthenticationProvider>(); };

        EXPECT_FALSE(registry.Register("", factory));
        EXPECT_FALSE(registry.Register("Test-Provider", factory));
        EXPECT_FALSE(registry.Register("invalid_name", factory));
        ASSERT_TRUE(registry.Register("test-provider", factory));
        EXPECT_FALSE(registry.Register("test-provider", factory));
        EXPECT_FALSE(registry.Create("missing"));
    }

    TEST(ServerApiAuthentication, ProviderReturnsIdentityAndChallenge)
    {
        TestAuthenticationProvider provider;
        ServerApi::AuthenticationRequest request;
        request.method = "GET";
        request.target = "/api/v1/server";
        request.headers.push_back({"X-User-Id", "realm-user-42"});

        ServerApi::AuthenticationResult const accepted = provider.Authenticate(request);
        ASSERT_TRUE(accepted.accepted);
        ASSERT_TRUE(accepted.subject.has_value());
        EXPECT_EQ(*accepted.subject, "realm-user-42");

        request.headers.clear();
        ServerApi::AuthenticationResult const rejected = provider.Authenticate(request);
        EXPECT_FALSE(rejected.accepted);
        EXPECT_EQ(rejected.challenge, "Test realm=\"server-api\"");
    }

    TEST(ServerApiAuthentication, NonLocalBindRequiresAuthentication)
    {
        ServerApi::NoAuthProvider noAuthProvider;
        TestAuthenticationProvider authenticatedProvider;

        EXPECT_TRUE(ServerApi::IsAuthenticationProviderAllowedForBind(noAuthProvider.RequiresAuthentication(), true));
        EXPECT_FALSE(ServerApi::IsAuthenticationProviderAllowedForBind(noAuthProvider.RequiresAuthentication(), false));
        EXPECT_TRUE(ServerApi::IsAuthenticationProviderAllowedForBind(
            authenticatedProvider.RequiresAuthentication(), false));
    }

    TEST(ServerApiAuthentication, RequestHeaderLookupIsCaseInsensitive)
    {
        ServerApi::AuthenticationRequest request;
        request.headers.push_back({"X-CUSTOM-IDENTITY", "user-1"});

        EXPECT_EQ(request.HeaderValue("x-custom-identity"), "user-1");
        EXPECT_TRUE(request.HeaderValue("authorization").empty());
    }
}
