/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "gtest/gtest.h"

#include "ServerApi/ModuleRegistry.h"

#include <optional>
#include <stdexcept>

namespace
{
    TEST(ServerApiModuleRegistry, RegistersAndReturnsCopyOfDescriptor)
    {
        ServerApi::ModuleRegistry registry;
        ASSERT_TRUE(registry.Register({"dungeon-clear", "1.0.0", {"dungeons", "runs"}}));

        std::vector<ServerApi::ModuleDescriptor> const modules = registry.Snapshot();
        ASSERT_EQ(modules.size(), 1U);
        EXPECT_EQ(modules.front().name, "dungeon-clear");
        EXPECT_EQ(modules.front().capabilities, std::vector<std::string>({"dungeons", "runs"}));
    }

    TEST(ServerApiModuleRegistry, RejectsDuplicateAndConflictingRegistration)
    {
        ServerApi::ModuleRegistry registry;
        ServerApi::ModuleDescriptor const descriptor{"raid-runner", "1.0.0", {"raids"}};

        EXPECT_TRUE(registry.Register(descriptor));
        EXPECT_FALSE(registry.Register(descriptor));
        EXPECT_FALSE(registry.Register({"raid-runner", "2.0.0", {"raids"}}));
        EXPECT_EQ(registry.Snapshot().size(), 1U);
    }

    TEST(ServerApiModuleRegistry, DispatchesOnlyInsideModuleNamespace)
    {
        ServerApi::ModuleRegistry registry;
        ASSERT_TRUE(registry.Register({"raid-runner", "1.0.0", {"raids"}},
            [](ServerApi::ModuleApiRequest const& request)
            {
                EXPECT_EQ(request.method, "GET");
                return std::optional<ServerApi::ModuleApiResponse>{
                    ServerApi::ModuleApiResponse{200, "OK", R"({"status":"ok"})", {}}};
            }));

        auto const response = registry.Dispatch({"GET", "/api/v1/mod/raid-runner/status",
            "/api/v1/mod/raid-runner/status"});
        ASSERT_TRUE(response.has_value());
        EXPECT_EQ(response->body, R"({"status":"ok"})");

        EXPECT_FALSE(registry.Dispatch({"GET", "/api/v1/mod/other/status", "/api/v1/mod/other/status"}));
        EXPECT_FALSE(registry.Dispatch({"GET", "/api/v1/raid-runner/status", "/api/v1/raid-runner/status"}));
    }

    TEST(ServerApiModuleRegistry, ConvertsHandlerExceptionToSafeResponse)
    {
        ServerApi::ModuleRegistry registry;
        ASSERT_TRUE(registry.Register({"unstable", "1.0.0", {"status"}},
            [](ServerApi::ModuleApiRequest const&) -> std::optional<ServerApi::ModuleApiResponse>
            {
                throw std::runtime_error("test failure");
            }));

        auto const response = registry.Dispatch({"GET", "/api/v1/mod/unstable/status",
            "/api/v1/mod/unstable/status"});
        ASSERT_TRUE(response.has_value());
        EXPECT_EQ(response->status, 500);
        EXPECT_EQ(response->body, R"({"error":{"code":"MODULE_HANDLER_FAILED"}})");
    }

    TEST(ServerApiModuleRegistry, RejectsEmptyNameAndSupportsUnregister)
    {
        ServerApi::ModuleRegistry registry;

        EXPECT_FALSE(registry.Register({"", "1.0.0", {}}));
        EXPECT_TRUE(registry.Register({"account", "1.0.0", {"accounts"}}));
        EXPECT_TRUE(registry.Unregister("account"));
        EXPECT_FALSE(registry.Unregister("account"));
        EXPECT_TRUE(registry.Snapshot().empty());
    }

    TEST(ServerApiModuleRegistry, RejectsInvalidNamesVersionsAndDuplicateCapabilities)
    {
        ServerApi::ModuleRegistry registry;

        EXPECT_FALSE(registry.Register({"Dungeon-Clear", "1.0.0", {}}));
        EXPECT_FALSE(registry.Register({"dungeon-clear", "", {}}));
        EXPECT_FALSE(registry.Register({"dungeon-clear", "1.0.0", {"runs", "runs"}}));
    }
}
