/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#ifndef SERVER_API_EVENT_BUS_H_
#define SERVER_API_EVENT_BUS_H_

#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

namespace ServerApi
{
    enum class EventPriority
    {
        Critical,
        Normal,
        Telemetry
    };

    struct ApiEvent
    {
        std::string type;
        int64_t timestampMilliseconds = 0;
        std::unordered_map<std::string, std::string> data;
        EventPriority priority = EventPriority::Normal;
    };

    using SubscriptionId = uint64_t;
    using EventHandler = std::function<void(ApiEvent const&)>;

    class EventBus
    {
    public:
        explicit EventBus(std::size_t maxQueueSize = 1000);
        ~EventBus();

        EventBus(EventBus const&) = delete;
        EventBus& operator=(EventBus const&) = delete;

        void Start();
        void Stop();

        bool Publish(ApiEvent event);
        SubscriptionId Subscribe(std::string eventPattern, EventHandler handler);
        void Unsubscribe(SubscriptionId subscriptionId);

        [[nodiscard]] std::size_t QueueSize() const;
        [[nodiscard]] uint64_t DroppedCount() const;

    private:
        struct Subscription
        {
            SubscriptionId id;
            std::string pattern;
            EventHandler handler;
        };

        void DispatchLoop();
        static bool Matches(std::string const& pattern, std::string const& eventType);

        std::size_t _maxQueueSize;
        mutable std::mutex _mutex;
        std::condition_variable _condition;
        std::deque<ApiEvent> _queue;
        std::vector<Subscription> _subscriptions;
        std::thread _thread;
        SubscriptionId _nextSubscriptionId = 1;
        uint64_t _droppedCount = 0;
        bool _running = false;
    };

    EventBus& GetEventBus();

    bool Publish(std::string eventType, std::unordered_map<std::string, std::string> data = {},
        EventPriority priority = EventPriority::Normal);
}

#endif
