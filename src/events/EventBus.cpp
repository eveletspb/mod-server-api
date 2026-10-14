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
        constexpr std::size_t MaxRdeStreamHistoryEvents = 8192;
        constexpr std::size_t MaxRdeStreamHistoryBytes = 16 * 1024 * 1024;

        int64_t NowMilliseconds()
        {
            auto const now = std::chrono::system_clock::now();
            return std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
        }

        std::string CreateRdeStreamEpoch()
        {
            auto const systemNow = std::chrono::system_clock::now().time_since_epoch();
            auto const steadyNow = std::chrono::steady_clock::now().time_since_epoch();
            auto const systemMicros = std::chrono::duration_cast<std::chrono::microseconds>(systemNow).count();
            auto const steadyNanos = std::chrono::duration_cast<std::chrono::nanoseconds>(steadyNow).count();
            return std::to_string(systemMicros) + "-" + std::to_string(steadyNanos);
        }

        std::size_t ApproximateEventBytes(ApiEvent const& event)
        {
            std::size_t bytes = event.type.size() + event.streamEpoch.size() + 96;
            for (auto const& [key, value] : event.data)
                bytes += key.size() + value.size() + 8;
            return bytes;
        }
    }

    EventBus::EventBus(std::size_t maxQueueSize)
        : _maxQueueSize(maxQueueSize), _rdeStreamEpoch(CreateRdeStreamEpoch()) { }

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

    void EventBus::SetMaxQueueSize(std::size_t maxQueueSize)
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        _maxQueueSize = maxQueueSize;
    }

    bool EventBus::Publish(ApiEvent event)
    {
        std::lock_guard<std::mutex> lock(_queueMutex);
        if (!_running)
            return false;

        bool const isRdeStreamEvent = event.type.rfind("rde.run.log.", 0) == 0;
        if (isRdeStreamEvent)
        {
            event.streamSequence = ++_rdeStreamSequence;
            event.streamEpoch = _rdeStreamEpoch;
        }

        if (event.timestampMilliseconds == 0)
            event.timestampMilliseconds = NowMilliseconds();

        if (isRdeStreamEvent)
        {
            std::size_t const eventBytes = ApproximateEventBytes(event);
            if (eventBytes <= MaxRdeStreamHistoryBytes)
            {
                _rdeStreamHistory.push_back(event);
                _rdeStreamHistoryBytes += eventBytes;
                while (_rdeStreamHistory.size() > MaxRdeStreamHistoryEvents ||
                    _rdeStreamHistoryBytes > MaxRdeStreamHistoryBytes)
                {
                    _rdeStreamHistoryBytes -= ApproximateEventBytes(_rdeStreamHistory.front());
                    _rdeStreamHistory.pop_front();
                }
            }
        }

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

    EventReplay EventBus::SubscribeFrom(std::vector<std::string> eventPatterns, EventHandler handler,
        std::string const& streamEpoch, uint64_t afterSequence, std::size_t maxEvents,
        std::size_t maxBytes)
    {
        EventReplay result;
        std::lock_guard<std::mutex> queueLock(_queueMutex);
        std::lock_guard<std::mutex> subscriptionLock(_subscriptionsMutex);

        bool const matchingEpoch = !streamEpoch.empty() && streamEpoch == _rdeStreamEpoch &&
            afterSequence <= _rdeStreamSequence;
        bool const historyCoversCursor = _rdeStreamHistory.empty()
            ? afterSequence == _rdeStreamSequence
            : afterSequence >= _rdeStreamHistory.front().streamSequence - 1;
        result.cursorAccepted = matchingEpoch && historyCoversCursor;
        if (matchingEpoch && maxEvents > 0 && maxBytes > 0)
        {
            std::size_t replayBytes = 0;
            for (auto event = _rdeStreamHistory.rbegin(); event != _rdeStreamHistory.rend(); ++event)
            {
                if (event->streamSequence <= afterSequence)
                    break;
                bool matches = std::any_of(eventPatterns.begin(), eventPatterns.end(), [&event](std::string const& pattern)
                {
                    return Matches(pattern, event->type);
                });
                if (!matches)
                    continue;

                std::size_t const eventBytes = ApproximateEventBytes(*event);
                if (result.events.size() >= maxEvents || eventBytes > maxBytes - replayBytes)
                    break;
                replayBytes += eventBytes;
                result.events.push_back(*event);
            }
            std::reverse(result.events.begin(), result.events.end());
        }

        for (std::string& pattern : eventPatterns)
        {
            SubscriptionId const id = _nextSubscriptionId++;
            _subscriptions.push_back({id, std::move(pattern), handler});
            result.subscriptions.push_back(id);
        }
        return result;
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
