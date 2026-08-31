#include "sequencer.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

constexpr std::size_t COMMAND_COUNT = 100;
constexpr auto PROGRESS_DEADLINE = std::chrono::seconds(2);

[[noreturn]] void exitSmokeTest(const int status) {
    std::cout.flush();
    std::cerr.flush();
    std::_Exit(status);
}

} // namespace

int main() {
    exchange::core::SharedQueue<exchange::sequencer::sequenceMessage> ingress(COMMAND_COUNT);
    exchange::core::SharedQueue<exchange::sequencer::sequenceMessage> matching(COMMAND_COUNT);
    exchange::sequencer::Sequencer sequencer(ingress, matching,
                                             exchange::sequencer::Sequencer::INTERNAL_ADMISSION_BYPASS);

    for (std::size_t index = 0; index < COMMAND_COUNT; ++index) {
        exchange::sequencer::sequenceMessage command{};
        command.configurationVersion = index + 1;
        command.type = exchange::sequencer::orderType::BUY;
        if (!ingress.push(command)) {
            std::cerr << "production-loop smoke ingress unexpectedly full\n";
            exitSmokeTest(EXIT_FAILURE);
        }
    }

    std::thread productionLoop([&sequencer]() { sequencer.run(); });
    productionLoop.detach();

    const auto deadline = std::chrono::steady_clock::now() + PROGRESS_DEADLINE;
    for (std::size_t index = 0; index < COMMAND_COUNT; ++index) {
        exchange::sequencer::sequenceMessage command{};
        while (!matching.pop(command)) {
            if (std::chrono::steady_clock::now() >= deadline) {
                std::cerr << "production-loop smoke exceeded FIFO progress deadline\n";
                exitSmokeTest(EXIT_FAILURE);
            }
            std::this_thread::yield();
        }
        if (command.configurationVersion != index + 1 || command.globalSequenceNumber.value() != index + 1) {
            std::cerr << "production-loop smoke observed non-FIFO sequencing\n";
            exitSmokeTest(EXIT_FAILURE);
        }
    }

    std::cout << "validated_commands=" << COMMAND_COUNT << '\n';
    exitSmokeTest(EXIT_SUCCESS);
}
