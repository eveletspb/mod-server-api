/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/WebSocketPublishPool.h"

#include "Log.h"

#include <boost/property_tree/json_parser.hpp>
#include <boost/property_tree/ptree.hpp>

#include <array>
#include <sstream>

namespace ServerApi
{
    WebSocketPublishPool::WebSocketPublishPool(std::size_t maxQueueSize)
        : _maxQueueSize(maxQueueSize) { }

    WebSocketPublishPool::~WebSocketPublishPool()
    {
        Stop();
    }

    void WebSocketPublishPool::Start()
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_running)
            return;

        _running = true;
        _workers.reserve(4);
        for (std::size_t index = 0; index < 4; ++index)
            _workers.emplace_back(&WebSocketPublishPool::WorkerLoop, this);
    }

    void WebSocketPublishPool::Stop()
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_running && _workers.empty())
                return;
            _running = false;
        }

        _condition.notify_all();
        for (std::thread& worker : _workers)
            if (worker.joinable())
                worker.join();
        _workers.clear();
    }

    bool WebSocketPublishPool::TryEnqueue(std::string message)
    {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            if (!_running || _outstanding >= _maxQueueSize)
            {
                ++_droppedCount;
                return false;
            }

            _queue.push_back({_nextSequence++, std::move(message)});
            ++_outstanding;
        }
        _condition.notify_one();
        return true;
    }

    std::size_t WebSocketPublishPool::QueueSize() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _outstanding;
    }

    std::optional<ApiEvent> WebSocketPublishPool::ParseMessage(std::string const& message)
    {
        try
        {
            std::istringstream input(message);
            boost::property_tree::ptree root;
            boost::property_tree::read_json(input, root);

            if (root.get<std::string>("type") != "publish")
                return std::nullopt;

            ApiEvent event;
            event.type = root.get<std::string>("event");
            if (event.type.empty() || event.type.size() > 128)
                return std::nullopt;

            auto const& data = root.get_child("data");
            for (auto const& [key, value] : data)
            {
                if (key.empty() || !value.empty())
                    return std::nullopt;
                event.data.emplace(key, value.data());
            }
            return event;
        }
        catch (...)
        {
            return std::nullopt;
        }
    }

    void WebSocketPublishPool::WorkerLoop()
    {
        while (true)
        {
            Task task;
            {
                std::unique_lock<std::mutex> lock(_mutex);
                _condition.wait(lock, [this]
                {
                    return !_running || !_queue.empty();
                });

                if (!_running && _queue.empty())
                    return;

                task = std::move(_queue.front());
                _queue.pop_front();
            }

            std::optional<ApiEvent> event = ParseMessage(task.message);
            std::lock_guard<std::mutex> lock(_mutex);
            if (!event)
                ++_invalidCount;
            _completed.emplace(task.sequence, std::move(event));

            while (true)
            {
                auto completed = _completed.find(_nextSequenceToPublish);
                if (completed == _completed.end())
                    break;

                if (completed->second && !GetEventBus().Publish(std::move(*completed->second)))
                    ++_droppedCount;
                _completed.erase(completed);
                ++_nextSequenceToPublish;
                --_outstanding;
            }
        }
    }
}
