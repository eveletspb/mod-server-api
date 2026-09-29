/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/CommandQueue.h"

#include "Log.h"

#include <chrono>
#include <exception>
#include <utility>

namespace ServerApi
{
    bool CommandQueue::Enqueue(std::function<void()> command)
    {
        std::lock_guard lock(_mutex);
        if (!command || _commands.size() >= _maxSize)
            return false;

        _commands.push(std::move(command));
        return true;
    }

    std::size_t CommandQueue::Drain(std::size_t maxCommands, std::chrono::milliseconds timeBudget)
    {
        auto const deadline = std::chrono::steady_clock::now() + timeBudget;
        std::size_t processed = 0;
        while (processed < maxCommands && std::chrono::steady_clock::now() < deadline)
        {
            std::function<void()> command;
            {
                std::lock_guard lock(_mutex);
                if (_commands.empty())
                    break;
                command = std::move(_commands.front());
                _commands.pop();
            }

            try
            {
                command();
            }
            catch (std::exception const& exception)
            {
                LOG_ERROR("server-api.commands", "World command failed: {}", exception.what());
            }
            catch (...)
            {
                LOG_ERROR("server-api.commands", "World command failed with an unknown exception");
            }
            ++processed;
        }
        return processed;
    }

    std::size_t CommandQueue::Size() const
    {
        std::lock_guard lock(_mutex);
        return _commands.size();
    }

    void CommandQueue::Clear()
    {
        std::lock_guard lock(_mutex);
        std::queue<std::function<void()>> empty;
        _commands.swap(empty);
    }

    CommandQueue& GetCommandQueue()
    {
        static CommandQueue queue;
        return queue;
    }
}
