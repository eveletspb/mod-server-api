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

        ServerApi::SubscriptionId const subscription = bus.Subscribe("test.*",
            [&mutex, &condition, &deliveries](ServerApi::ApiEvent const&)
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

    TEST(ServerApiEventBus, HandlerFailuresDoNotStopDispatcher)
    {
        ServerApi::EventBus bus;
        std::mutex mutex;
        std::condition_variable condition;
        uint32_t deliveries = 0;

        bus.Subscribe("test.*", [&mutex, &condition, &deliveries](ServerApi::ApiEvent const& event)
        {
            if (event.type == "test.failure")
                throw std::runtime_error("expected test failure");
            if (event.type == "test.unknown-failure")
                throw 1;

            std::lock_guard lock(mutex);
            ++deliveries;
            condition.notify_all();
        });

        bus.Start();
        ASSERT_TRUE(bus.Publish({"test.failure"}));
        ASSERT_TRUE(bus.Publish({"test.unknown-failure"}));
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

        bus.Subscribe("test.*", [&mutex, &condition, &firstStarted, &releaseFirst,
            &received](ServerApi::ApiEvent const& event)
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
        EXPECT_EQ(bus.DroppedCount(), 1);
    }

    TEST(ServerApiEventBus, RdeStreamSequenceExposesDroppedEvents)
    {
        ServerApi::EventBus bus(1);
        std::mutex mutex;
        std::condition_variable condition;
        bool firstStarted = false;
        bool releaseFirst = false;
        std::vector<uint64_t> receivedSequences;

        bus.Subscribe("rde.run.log.*", [&mutex, &condition, &firstStarted, &releaseFirst,
            &receivedSequences](ServerApi::ApiEvent const& event)
        {
            std::unique_lock lock(mutex);
            receivedSequences.push_back(event.streamSequence);
            condition.notify_all();
            if (event.streamSequence == 1)
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
        ASSERT_TRUE(bus.Publish({"rde.run.log.first", 0, {}, ServerApi::EventPriority::Telemetry}));
        {
            std::unique_lock lock(mutex);
            ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&firstStarted]
            {
                return firstStarted;
            }));
        }

        ASSERT_TRUE(bus.Publish({"rde.run.log.queued", 0, {}, ServerApi::EventPriority::Telemetry}));
        EXPECT_FALSE(bus.Publish({"rde.run.log.dropped", 0, {}, ServerApi::EventPriority::Telemetry}));
        {
            std::lock_guard lock(mutex);
            releaseFirst = true;
        }
        condition.notify_all();
        {
            std::unique_lock lock(mutex);
            ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&receivedSequences]
            {
                return receivedSequences.size() == 2;
            }));
        }
        ASSERT_TRUE(bus.Publish({"rde.run.log.after_drop", 0, {}, ServerApi::EventPriority::Telemetry}));
        bus.Stop();

        ASSERT_EQ(receivedSequences.size(), 3);
        EXPECT_EQ(receivedSequences[0], 1);
        EXPECT_EQ(receivedSequences[1], 2);
        EXPECT_EQ(receivedSequences[2], 4);
    }

    TEST(ServerApiEventBus, ReplaysLatestEventsFromMatchingStreamEpoch)
    {
        ServerApi::EventBus bus(16);
        std::mutex mutex;
        std::condition_variable condition;
        std::vector<ServerApi::ApiEvent> received;
        bus.Subscribe("rde.run.log.*", [&mutex, &condition, &received](ServerApi::ApiEvent const& event)
        {
            std::lock_guard lock(mutex);
            received.push_back(event);
            condition.notify_all();
        });
        bus.Start();
        ASSERT_TRUE(bus.Publish({"rde.run.log.first"}));
        ASSERT_TRUE(bus.Publish({"rde.run.log.second"}));
        {
            std::unique_lock lock(mutex);
            ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(2), [&received]
            {
                return received.size() == 2;
            }));
        }

        std::vector<ServerApi::ApiEvent> liveAfterReplay;
        ServerApi::EventReplay replay = bus.SubscribeFrom({"rde.run.log.*"},
            [&liveAfterReplay](ServerApi::ApiEvent const& event)
            {
                liveAfterReplay.push_back(event);
            }, received.front().streamEpoch, received.front().streamSequence, 1, 1024);

        ASSERT_TRUE(replay.cursorAccepted);
        ASSERT_EQ(replay.events.size(), 1);
        EXPECT_EQ(replay.events.front().streamSequence, received.back().streamSequence);
        EXPECT_EQ(replay.events.front().type, "rde.run.log.second");
        ASSERT_EQ(replay.subscriptions.size(), 1);

        ASSERT_TRUE(bus.Publish({"rde.run.log.third"}));
        bus.Stop();
        ASSERT_EQ(liveAfterReplay.size(), 1);
        EXPECT_EQ(liveAfterReplay.front().type, "rde.run.log.third");
        bus.Unsubscribe(replay.subscriptions.front());
    }

    TEST(ServerApiEventBus, ReplaysBacklogLargerThanDefaultSseQueue)
    {
        constexpr uint64_t EventCount = 5000;
        ServerApi::EventBus bus(EventCount + 1);
        std::mutex mutex;
        std::condition_variable condition;
        std::vector<ServerApi::ApiEvent> received;
        bus.Subscribe("rde.run.log.*", [&mutex, &condition, &received](ServerApi::ApiEvent const& event)
        {
            std::lock_guard lock(mutex);
            received.push_back(event);
            condition.notify_all();
        });
        bus.Start();
        for (uint64_t index = 0; index < EventCount; ++index)
            ASSERT_TRUE(bus.Publish({"rde.run.log." + std::to_string(index)}));

        {
            std::unique_lock lock(mutex);
            ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(5), [&received]
            {
                return received.size() == EventCount;
            }));
        }

        ServerApi::EventReplay replay = bus.SubscribeFrom({"rde.run.log.*"}, [](ServerApi::ApiEvent const&) { },
            received.front().streamEpoch, 0, 8192, 16 * 1024 * 1024);
        bus.Stop();

        ASSERT_TRUE(replay.cursorAccepted);
        ASSERT_EQ(replay.events.size(), EventCount);
        EXPECT_EQ(replay.events.front().streamSequence, 1);
        EXPECT_EQ(replay.events.back().streamSequence, EventCount);
    }

    TEST(ServerApiEventBus, MarksCursorOlderThanRetainedHistoryIncomplete)
    {
        constexpr uint64_t EventCount = 8193;
        ServerApi::EventBus bus(EventCount + 1);
        std::mutex mutex;
        std::condition_variable condition;
        std::vector<ServerApi::ApiEvent> received;
        bus.Subscribe("rde.run.log.*", [&mutex, &condition, &received](ServerApi::ApiEvent const& event)
        {
            std::lock_guard lock(mutex);
            received.push_back(event);
            condition.notify_all();
        });
        bus.Start();
        for (uint64_t index = 0; index < EventCount; ++index)
            ASSERT_TRUE(bus.Publish({"rde.run.log." + std::to_string(index)}));

        {
            std::unique_lock lock(mutex);
            ASSERT_TRUE(condition.wait_for(lock, std::chrono::seconds(5), [&received]
            {
                return received.size() == EventCount;
            }));
        }

        ServerApi::EventReplay replay = bus.SubscribeFrom({"rde.run.log.*"}, [](ServerApi::ApiEvent const&) { },
            received.front().streamEpoch, 0, 8192, 16 * 1024 * 1024);
        bus.Stop();

        EXPECT_FALSE(replay.cursorAccepted);
        ASSERT_EQ(replay.events.size(), 8192);
        EXPECT_EQ(replay.events.front().streamSequence, 2);
        EXPECT_EQ(replay.events.back().streamSequence, EventCount);
    }

    TEST(ServerApiEventBus, DoesNotReplayCursorFromAnotherServerEpoch)
    {
        ServerApi::EventBus bus(8);
        bus.Start();
        ASSERT_TRUE(bus.Publish({"rde.run.log.current"}));
        ServerApi::EventReplay replay = bus.SubscribeFrom({"rde.run.log.*"}, [](ServerApi::ApiEvent const&) { },
            "previous-server-process", 1, 8, 1024);
        EXPECT_FALSE(replay.cursorAccepted);
        EXPECT_TRUE(replay.events.empty());
        ASSERT_EQ(replay.subscriptions.size(), 1);
        bus.Unsubscribe(replay.subscriptions.front());
        bus.Stop();
    }
}
