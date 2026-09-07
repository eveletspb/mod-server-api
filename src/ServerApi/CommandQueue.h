/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef SERVER_API_COMMAND_QUEUE_H_
#define SERVER_API_COMMAND_QUEUE_H_

#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>

namespace ServerApi
{
    class CommandQueue
    {
    public:
        explicit CommandQueue(std::size_t maxSize = 1000) : _maxSize(maxSize) { }

        bool Enqueue(std::function<void()> command);
        std::size_t Drain(std::size_t maxCommands);
        void Clear();
        std::size_t Size() const;

    private:
        std::size_t _maxSize;
        mutable std::mutex _mutex;
        std::queue<std::function<void()>> _commands;
    };

    CommandQueue& GetCommandQueue();
}

#endif
