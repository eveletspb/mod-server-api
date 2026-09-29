/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license.
 */

#include "ServerApi/CommandQueue.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <thread>

namespace ServerApi
{
    TEST(CommandQueueTest, StopsDrainingAfterTimeBudget)
    {
        CommandQueue queue;
        uint32_t executed = 0;
        ASSERT_TRUE(queue.Enqueue([&executed]
        {
            ++executed;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }));
        ASSERT_TRUE(queue.Enqueue([&executed]
        {
            ++executed;
        }));

        EXPECT_EQ(queue.Drain(100, std::chrono::milliseconds(1)), 1u);
        EXPECT_EQ(executed, 1u);
        EXPECT_EQ(queue.Size(), 1u);
    }
}
