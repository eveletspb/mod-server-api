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
        std::string streamEpoch;
        uint64_t streamSequence = 0;
    };

    using SubscriptionId = uint64_t;
    using EventHandler = std::function<void(ApiEvent const&)>;

    struct EventReplay
    {
        std::vector<ApiEvent> events;
        std::vector<SubscriptionId> subscriptions;
        bool cursorAccepted = false;
    };

    class EventBus
    {
    public:
        explicit EventBus(std::size_t maxQueueSize = 1000);
        ~EventBus();

        EventBus(EventBus const&) = delete;
        EventBus& operator=(EventBus const&) = delete;

        void Start();
        void Stop();
        void SetMaxQueueSize(std::size_t maxQueueSize);

        bool Publish(ApiEvent event);
        SubscriptionId Subscribe(std::string eventPattern, EventHandler handler);
        EventReplay SubscribeFrom(std::vector<std::string> eventPatterns, EventHandler handler,
            std::string const& streamEpoch, uint64_t afterSequence, std::size_t maxEvents,
            std::size_t maxBytes);
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
        mutable std::mutex _queueMutex;
        mutable std::mutex _subscriptionsMutex;
        std::condition_variable _condition;
        std::deque<ApiEvent> _queue;
        std::vector<Subscription> _subscriptions;
        std::thread _thread;
        SubscriptionId _nextSubscriptionId = 1;
        uint64_t _droppedCount = 0;
        std::string _rdeStreamEpoch;
        uint64_t _rdeStreamSequence = 0;
        std::deque<ApiEvent> _rdeStreamHistory;
        std::size_t _rdeStreamHistoryBytes = 0;
        bool _running = false;
    };

    EventBus& GetEventBus();

    bool Publish(std::string eventType, std::unordered_map<std::string, std::string> data = {},
        EventPriority priority = EventPriority::Normal);
}

#endif
