#pragma once

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <thread>

namespace exchange::core::task {

class AdaptiveIdle final {
public:
    enum class ActionKind {
        YIELD,
        SLEEP,
    };

    struct Action {
        ActionKind kind;
        std::chrono::microseconds sleepDuration;
    };

    static constexpr std::size_t YIELD_ATTEMPTS = 64;
    static constexpr std::chrono::microseconds INITIAL_SLEEP{1};
    static constexpr std::chrono::microseconds MAX_SLEEP{1'000};

    [[nodiscard]] Action nextAction() noexcept {
        ++idleAttempts_;
        if (idleAttempts_ <= YIELD_ATTEMPTS) {
            return Action{ActionKind::YIELD, std::chrono::microseconds::zero()};
        }

        if (sleepDuration_ == std::chrono::microseconds::zero()) {
            sleepDuration_ = INITIAL_SLEEP;
        } else {
            sleepDuration_ = std::min(sleepDuration_ * 2, MAX_SLEEP);
        }
        return Action{ActionKind::SLEEP, sleepDuration_};
    }

    void wait() {
        const Action action = nextAction();
        if (action.kind == ActionKind::YIELD) {
            std::this_thread::yield();
            return;
        }
        std::this_thread::sleep_for(action.sleepDuration);
    }

    void reset() noexcept {
        idleAttempts_ = 0;
        sleepDuration_ = std::chrono::microseconds::zero();
    }

    [[nodiscard]] std::size_t idleAttempts() const noexcept {
        return idleAttempts_;
    }

private:
    std::size_t idleAttempts_ = 0;
    std::chrono::microseconds sleepDuration_{0};
};

} // namespace exchange::core::task
