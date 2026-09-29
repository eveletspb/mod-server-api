/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/ModuleRegistry.h"

#include <algorithm>
#include <cctype>
#include <utility>

namespace ServerApi
{
    namespace
    {
        bool IsValidName(std::string_view value)
        {
            if (value.empty())
                return false;
            for (char character : value)
            {
                if (!std::islower(static_cast<unsigned char>(character)) &&
                    !std::isdigit(static_cast<unsigned char>(character)) &&
                    character != '-')
                    return false;
            }
            return true;
        }

        bool IsValidVersion(std::string_view value)
        {
            if (value.empty())
                return false;
            return std::all_of(value.begin(), value.end(), [](char character)
            {
                return !std::iscntrl(static_cast<unsigned char>(character));
            });
        }

        bool IsValidCapability(std::string_view value)
        {
            if (value.empty())
                return false;
            return std::all_of(value.begin(), value.end(), [](char character)
            {
                return std::islower(static_cast<unsigned char>(character)) ||
                    std::isdigit(static_cast<unsigned char>(character)) ||
                    character == '.' || character == '-';
            });
        }
    }

    bool ModuleRegistry::Register(ModuleDescriptor descriptor, ModuleApiHandler handler)
    {
        for (std::size_t index = 0; index < descriptor.capabilities.size(); ++index)
            for (std::size_t next = index + 1; next < descriptor.capabilities.size(); ++next)
                if (descriptor.capabilities[index] == descriptor.capabilities[next])
                    return false;

        if (!IsValidName(descriptor.name) || !IsValidVersion(descriptor.version) ||
            std::any_of(descriptor.capabilities.begin(), descriptor.capabilities.end(),
                [](std::string const& capability)
        {
            return !IsValidCapability(capability);
        }))
            return false;

        std::lock_guard<std::mutex> lock(_mutex);
        auto const existing = std::find_if(_modules.begin(), _modules.end(),
            [&descriptor](Registration const& registration)
        {
            return registration.descriptor.name == descriptor.name;
        });
        if (existing != _modules.end())
            return false;

        _modules.push_back({std::move(descriptor), std::move(handler)});
        return true;
    }

    bool ModuleRegistry::Unregister(std::string_view name)
    {
        std::lock_guard<std::mutex> lock(_mutex);
        auto const existing = std::find_if(_modules.begin(), _modules.end(),
            [name](Registration const& registration)
        {
            return registration.descriptor.name == name;
        });
        if (existing == _modules.end())
            return false;

        _modules.erase(existing);
        return true;
    }

    std::vector<ModuleDescriptor> ModuleRegistry::Snapshot() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::vector<ModuleDescriptor> result;
        result.reserve(_modules.size());
        for (Registration const& registration : _modules)
            result.push_back(registration.descriptor);
        return result;
    }

    std::optional<ModuleApiResponse> ModuleRegistry::Dispatch(ModuleApiRequest const& request) const
    {
        std::string const prefix = "/api/v1/mod/";
        if (!request.path.starts_with(prefix))
            return std::nullopt;

        std::size_t const nameEnd = request.path.find('/', prefix.size());
        std::string_view const requestPath = request.path;
        std::string_view const name = requestPath.substr(prefix.size(),
            nameEnd == std::string::npos ? std::string::npos : nameEnd - prefix.size());

        ModuleApiHandler handler;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            auto const existing = std::find_if(_modules.begin(), _modules.end(),
                [name](Registration const& registration)
            {
                return registration.descriptor.name == name;
            });
            if (existing == _modules.end() || !existing->handler)
                return std::nullopt;
            handler = existing->handler;
        }

        try
        {
            return handler(request);
        }
        catch (...)
        {
            return ModuleApiResponse{
                500, "Internal Server Error", R"({"error":{"code":"MODULE_HANDLER_FAILED"}})", {}};
        }
    }

    ModuleRegistry& GetModuleRegistry()
    {
        static ModuleRegistry registry;
        return registry;
    }
}
