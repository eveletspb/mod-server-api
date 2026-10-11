/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "gtest/gtest.h"

#include "ServerApi/EventBus.h"
#include "ServerApi/WebSocketPublishPool.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

namespace
{
    TEST(ServerApiWebSocketPublishPool, ParsesStringValuedPublishMessages)
    {
        auto event = ServerApi::WebSocketPublishPool::ParseMessage(
            R"({"type":"publish","event":"provider.metric","data":{"name":"cpu","value":"42"}})");

        ASSERT_TRUE(event);
        EXPECT_EQ(event->type, "provider.metric");
        EXPECT_EQ(event->data.at("name"), "cpu");
        EXPECT_EQ(event->data.at("value"), "42");
    }

    TEST(ServerApiWebSocketPublishPool, RejectsMalformedAndUnsupportedMessages)
    {
        EXPECT_FALSE(ServerApi::WebSocketPublishPool::ParseMessage("{"));
        EXPECT_FALSE(ServerApi::WebSocketPublishPool::ParseMessage(
            R"({"type":"subscribe","events":["player.*"]})"));
        EXPECT_FALSE(ServerApi::WebSocketPublishPool::ParseMessage(
            R"({"type":"publish","event":"provider.metric","data":{"value":{"nested":"no"}}})"));
    }

    TEST(ServerApiWebSocketPublishPool, PublishesParallelWorkInAcceptedOrder)
    {
        ServerApi::EventBus& bus = ServerApi::GetEventBus();
        bus.Stop();
        bus.SetMaxQueueSize(256);

        std::mutex mutex;
        std::condition_variable condition;
        std::vector<std::string> received;
        ServerApi::SubscriptionId subscription = bus.Subscribe("provider.*",
            [&mutex, &condition, &received](ServerApi::ApiEvent const& event)
        {
            std::lock_guard<std::mutex> lock(mutex);
            received.push_back(event.data.at("sequence"));
            condition.notify_all();
        });

        bus.Start();
        ServerApi::WebSocketPublishPool pool(256);
        pool.Start();
        constexpr uint32_t EventCount = 128;
        for (uint32_t index = 0; index < EventCount; ++index)
        {
            std::string message = R"({"type":"publish","event":"provider.test","data":{"sequence":")";
            message += std::to_string(index) + R"("}})";
            if (!pool.TryEnqueue(std::move(message)))
                ADD_FAILURE() << "publication queue rejected message " << index;
        }

        bool receivedAll = false;
        {
            std::unique_lock<std::mutex> lock(mutex);
            receivedAll = condition.wait_for(lock, std::chrono::seconds(3), [&received]
            {
                return received.size() == EventCount;
            });
        }

        pool.Stop();
        bus.Unsubscribe(subscription);
        bus.Stop();
        EXPECT_TRUE(receivedAll);
        ASSERT_EQ(received.size(), EventCount);
        for (uint32_t index = 0; index < EventCount; ++index)
            EXPECT_EQ(received[index], std::to_string(index));
    }

    TEST(ServerApiWebSocketPublishPool, DropsMessagesWhenQueueCapacityIsFull)
    {
        ServerApi::WebSocketPublishPool pool(0);
        pool.Start();
        EXPECT_FALSE(pool.TryEnqueue(R"({"type":"publish","event":"provider.test","data":{}})"));
        EXPECT_EQ(pool.DroppedCount(), 1u);
        pool.Stop();
    }
}
