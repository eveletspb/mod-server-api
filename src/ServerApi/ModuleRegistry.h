/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef SERVER_API_MODULE_REGISTRY_H_
#define SERVER_API_MODULE_REGISTRY_H_

#include "ServerApi/Authentication.h"

#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ServerApi
{
    struct ModuleDescriptor
    {
        std::string name;
        std::string version;
        std::vector<std::string> capabilities;
    };

    struct ModuleApiRequest
    {
        std::string method;
        std::string target;
        std::string path;
        std::optional<AuthIdentity> identity;
    };

    struct ModuleApiResponse
    {
        uint16_t status = 200;
        std::string reason = "OK";
        std::string body;
        std::string headers;
    };

    using ModuleApiHandler = std::function<std::optional<ModuleApiResponse>(ModuleApiRequest const&)>;

    class ModuleRegistry
    {
    public:
        ModuleRegistry() = default;

        ModuleRegistry(ModuleRegistry const&) = delete;
        ModuleRegistry& operator=(ModuleRegistry const&) = delete;

        // A module owns only its /api/v1/mod/<name> namespace. Registration
        // with the same name is rejected, so two modules cannot silently
        // replace one another's API handler.
        bool Register(ModuleDescriptor descriptor, ModuleApiHandler handler = {});
        bool Unregister(std::string_view name);

        [[nodiscard]] std::vector<ModuleDescriptor> Snapshot() const;
        [[nodiscard]] std::optional<ModuleApiResponse> Dispatch(ModuleApiRequest const& request) const;

    private:
        mutable std::mutex _mutex;
        struct Registration
        {
            ModuleDescriptor descriptor;
            ModuleApiHandler handler;
        };
        std::vector<Registration> _modules;
    };

    ModuleRegistry& GetModuleRegistry();
}

#endif
