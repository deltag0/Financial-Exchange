#include "adaptive_idle.hpp"

#include <gtest/gtest.h>

namespace exchange::core::task {

TEST(AdaptiveIdleTest, YieldsBeforeSleepingAndCapsSleepDuration) {
    AdaptiveIdle idle;

    for (std::size_t attempt = 0; attempt < AdaptiveIdle::YIELD_ATTEMPTS; ++attempt) {
        const AdaptiveIdle::Action action = idle.nextAction();
        EXPECT_EQ(action.kind, AdaptiveIdle::ActionKind::YIELD);
        EXPECT_EQ(action.sleepDuration, std::chrono::microseconds::zero());
    }

    EXPECT_EQ(idle.nextAction().sleepDuration, AdaptiveIdle::INITIAL_SLEEP);
    for (std::size_t attempt = 0; attempt < 20; ++attempt) {
        const AdaptiveIdle::Action action = idle.nextAction();
        EXPECT_EQ(action.kind, AdaptiveIdle::ActionKind::SLEEP);
        EXPECT_LE(action.sleepDuration, AdaptiveIdle::MAX_SLEEP);
    }
    EXPECT_EQ(idle.nextAction().sleepDuration, AdaptiveIdle::MAX_SLEEP);
}

TEST(AdaptiveIdleTest, ProgressResetRestartsAtYieldWithoutSleeping) {
    AdaptiveIdle idle;
    for (std::size_t attempt = 0; attempt <= AdaptiveIdle::YIELD_ATTEMPTS; ++attempt) {
        static_cast<void>(idle.nextAction());
    }
    ASSERT_GT(idle.idleAttempts(), AdaptiveIdle::YIELD_ATTEMPTS);

    idle.reset();

    EXPECT_EQ(idle.idleAttempts(), 0u);
    const AdaptiveIdle::Action action = idle.nextAction();
    EXPECT_EQ(action.kind, AdaptiveIdle::ActionKind::YIELD);
    EXPECT_EQ(action.sleepDuration, std::chrono::microseconds::zero());
}

} // namespace exchange::core::task
