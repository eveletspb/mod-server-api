/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "gtest/gtest.h"

#include "ServerApi/EventBus.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
    TEST(ServerApiEventBus, DoesNotPublishBeforeStart)
    {
        ServerApi::EventBus bus;

        EXPECT_FALSE(bus.Publish({"server.started"}));
        bus.Start();
        EXPECT_TRUE(bus.Publish({"server.started"}));
        bus.Stop();
        EXPECT_FALSE(bus.Publish({"server.stopped"}));
    }

    TEST(ServerApiEventBus, DispatchesExactAndWildcardSubscriptions)
    {
        ServerApi::EventBus bus;
        std::mutex mutex;
        std::condition_variable condition;
        std::vector<std::string> received;

        bus.Subscribe("player.*", [&mutex, &condition, &received](ServerApi::ApiEvent const& event)
        {
            std::lock_guard lock(mutex);
            received.push_back(event.type);
            condition.notify_all();
        });
        bus.Subscribe("server.started", [&mutex, &condition, &received](ServerApi::ApiEvent const& event)
        {
            std::lock_guard lock(mutex);
            received.push_back(event.type);
            condition.notify_all();
        });

        bus.Start();
        ASSERT_TRUE(bus.Publish({"player.login", 0, {{"guid", "42"}}}));
        ASSERT_TRUE(bus.Publish({"server.started"}));

        std::unique_lock lock(mutex);
        ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&received]
        {
            return received.size() == 2;
        }));
        lock.unlock();
        bus.Stop();

        EXPECT_EQ(received[0], "player.login");
        EXPECT_EQ(received[1], "server.started");
    }

    TEST(ServerApiEventBus, UnsubscribeStopsFutureDelivery)
    {
        ServerApi::EventBus bus;
        std::mutex mutex;
        std::condition_variable condition;
        uint32_t deliveries = 0;

        ServerApi::SubscriptionId const subscription = bus.Subscribe("test.*", [&mutex, &condition, &deliveries](ServerApi::ApiEvent const&)
        {
            std::lock_guard lock(mutex);
            ++deliveries;
            condition.notify_all();
        });

        bus.Start();
        ASSERT_TRUE(bus.Publish({"test.first"}));
        {
            std::unique_lock lock(mutex);
            ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&deliveries]
            {
                return deliveries == 1;
            }));
        }

        bus.Unsubscribe(subscription);
        ASSERT_TRUE(bus.Publish({"test.second"}));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        bus.Stop();

        EXPECT_EQ(deliveries, 1);
    }

    TEST(ServerApiEventBus, HandlerFailureDoesNotStopDispatcher)
    {
        ServerApi::EventBus bus;
        std::mutex mutex;
        std::condition_variable condition;
        uint32_t deliveries = 0;

        bus.Subscribe("test.*", [&mutex, &condition, &deliveries](ServerApi::ApiEvent const& event)
        {
            if (event.type == "test.failure")
                throw std::runtime_error("expected test failure");

            std::lock_guard lock(mutex);
            ++deliveries;
            condition.notify_all();
        });

        bus.Start();
        ASSERT_TRUE(bus.Publish({"test.failure"}));
        ASSERT_TRUE(bus.Publish({"test.success"}));

        std::unique_lock lock(mutex);
        ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&deliveries]
        {
            return deliveries == 1;
        }));
        lock.unlock();
        bus.Stop();

        EXPECT_EQ(deliveries, 1);
    }

    TEST(ServerApiEventBus, PreservesNormalEventsWhenTelemetryFillsQueue)
    {
        ServerApi::EventBus bus(1);
        std::mutex mutex;
        std::condition_variable condition;
        bool firstStarted = false;
        bool releaseFirst = false;
        std::vector<std::string> received;

        bus.Subscribe("test.*", [&mutex, &condition, &firstStarted, &releaseFirst, &received](ServerApi::ApiEvent const& event)
        {
            std::unique_lock lock(mutex);
            received.push_back(event.type);
            if (event.type == "test.first")
            {
                firstStarted = true;
                condition.notify_all();
                condition.wait(lock, [&releaseFirst]
                {
                    return releaseFirst;
                });
            }
        });

        bus.Start();
        ASSERT_TRUE(bus.Publish({"test.first"}));
        {
            std::unique_lock lock(mutex);
            ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&firstStarted]
            {
                return firstStarted;
            }));
        }

        ASSERT_TRUE(bus.Publish({"test.telemetry", 0, {}, ServerApi::EventPriority::Telemetry}));
        ASSERT_TRUE(bus.Publish({"test.normal"}));

        {
            std::lock_guard lock(mutex);
            releaseFirst = true;
        }
        condition.notify_all();
        bus.Stop();

        ASSERT_EQ(received.size(), 2);
        EXPECT_EQ(received[0], "test.first");
        EXPECT_EQ(received[1], "test.normal");
        EXPECT_EQ(bus.DroppedCount(), 0);
    }
}
