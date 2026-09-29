/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/EventBus.h"

#include "Log.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <exception>
#include <mutex>
#include <thread>

namespace ServerApi
{
    namespace
    {
        int64_t NowMilliseconds()
        {
            auto const now = std::chrono::system_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        }
    }

    EventBus::EventBus(std::size_t maxQueueSize) : _maxQueueSize(maxQueueSize) { }

    EventBus::~EventBus()
    {
        Stop();
    }

    void EventBus::Start()
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        if (_running)
            return;

        _running = true;
        _thread = std::thread(&EventBus::DispatchLoop, this);
    }

    void EventBus::Stop()
    {
        {
            std::lock_guard<std::mutex> lock(_queueMutex);
            if (!_running && !_thread.joinable())
                return;

            _running = false;
        }

        _condition.notify_all();
        if (_thread.joinable())
            _thread.join();

        std::lock_guard<std::mutex> lock(_queueMutex);
        _queue.clear();
    }

    bool EventBus::Publish(ApiEvent event)
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        if (!_running)
            return false;

        if (_queue.size() >= _maxQueueSize)
        {
            if (event.priority == EventPriority::Telemetry)
            {
                ++_droppedCount;
                return false;
            }

            auto telemetry = std::find_if(_queue.begin(), _queue.end(), [](ApiEvent const& queued)
            {
                return queued.priority == EventPriority::Telemetry;
            });
            if (telemetry == _queue.end())
            {
                ++_droppedCount;
                return false;
            }
            _queue.erase(telemetry);
            ++_droppedCount;
        }

        if (event.timestampMilliseconds == 0)
            event.timestampMilliseconds = NowMilliseconds();

        _queue.push_back(std::move(event));
        _condition.notify_one();
        return true;
    }

    SubscriptionId EventBus::Subscribe(std::string eventPattern, EventHandler handler)
    {
        std::lock_guard<std::mutex> lock(_subscriptionsMutex);
        SubscriptionId const id = _nextSubscriptionId++;
        _subscriptions.push_back({id, std::move(eventPattern), std::move(handler)});
        return id;
    }

    void EventBus::Unsubscribe(SubscriptionId subscriptionId)
    {
        std::lock_guard<std::mutex> lock(_subscriptionsMutex);
        _subscriptions.erase(std::remove_if(_subscriptions.begin(), _subscriptions.end(),
            [subscriptionId](Subscription const& subscription)
        {
            return subscription.id == subscriptionId;
        }), _subscriptions.end());
    }

    std::size_t EventBus::QueueSize() const
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        return _queue.size();
    }

    uint64_t EventBus::DroppedCount() const
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        return _droppedCount;
    }

    void EventBus::DispatchLoop()
    {
        while (true)
        {
            ApiEvent event;
            std::vector<EventHandler> handlers;

            {
                std::unique_lock<std::mutex> lock(_queueMutex);
                _condition.wait(lock, [this]
                {
                    return !_running || !_queue.empty();
                });

                if (!_running && _queue.empty())
                    return;

                event = std::move(_queue.front());
                _queue.pop_front();
            }

            {
                std::lock_guard<std::mutex> lock(_subscriptionsMutex);
                for (Subscription const& subscription : _subscriptions)
                    if (Matches(subscription.pattern, event.type))
                        handlers.push_back(subscription.handler);
            }

            for (EventHandler const& handler : handlers)
            {
                try
                {
                    handler(event);
                }
                catch (std::exception const& exception)
                {
                    LOG_ERROR("server-api.events", "Event handler failed for {}: {}", event.type, exception.what());
                }
                catch (...)
                {
                    LOG_ERROR("server-api.events", "Event handler failed for {} with an unknown exception", event.type);
                }
            }
        }
    }

    bool EventBus::Matches(std::string const& pattern, std::string const& eventType)
    {
        if (pattern == eventType)
            return true;

        if (pattern.size() > 1 && pattern.back() == '*')
            return eventType.compare(0, pattern.size() - 1, pattern, 0, pattern.size() - 1) == 0;

        return false;
    }

    EventBus& GetEventBus()
    {
        static EventBus eventBus;
        return eventBus;
    }

    bool Publish(std::string eventType, std::unordered_map<std::string, std::string> data, EventPriority priority)
    {
        return GetEventBus().Publish({std::move(eventType), 0, std::move(data), priority});
    }
}
