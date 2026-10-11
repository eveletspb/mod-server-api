/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef SERVER_API_WEBSOCKET_PUBLISH_POOL_H_
#define SERVER_API_WEBSOCKET_PUBLISH_POOL_H_

#include "ServerApi/EventBus.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace ServerApi
{
    class WebSocketPublishPool
    {
    public:
        explicit WebSocketPublishPool(std::size_t maxQueueSize);
        ~WebSocketPublishPool();

        WebSocketPublishPool(WebSocketPublishPool const&) = delete;
        WebSocketPublishPool& operator=(WebSocketPublishPool const&) = delete;

        void Start();
        void Stop();
        bool TryEnqueue(std::string message);

        [[nodiscard]] std::size_t QueueSize() const;
        [[nodiscard]] uint64_t DroppedCount() const { return _droppedCount.load(); }
        [[nodiscard]] uint64_t InvalidCount() const { return _invalidCount.load(); }

        static std::optional<ApiEvent> ParseMessage(std::string const& message);

    private:
        struct Task
        {
            uint64_t sequence;
            std::string message;
        };

        void WorkerLoop();

        std::size_t _maxQueueSize;
        mutable std::mutex _mutex;
        std::condition_variable _condition;
        std::deque<Task> _queue;
        std::map<uint64_t, std::optional<ApiEvent>> _completed;
        std::vector<std::thread> _workers;
        uint64_t _nextSequence = 0;
        uint64_t _nextSequenceToPublish = 0;
        std::size_t _outstanding = 0;
        bool _running = false;
        std::atomic_uint64_t _droppedCount = 0;
        std::atomic_uint64_t _invalidCount = 0;
    };
}

#endif
