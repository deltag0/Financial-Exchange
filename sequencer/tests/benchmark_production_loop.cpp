#include "sequencer.hpp"

#include <chrono>
#include <cstdlib>
#include <iostream>
#include <thread>

namespace {

constexpr std::size_t COMMAND_COUNT = 100;

[[noreturn]] void exitBenchmark(const int status) {
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
        command.id = index + 1;
        command.type = exchange::sequencer::orderType::BUY;
        if (!ingress.push(command)) {
            std::cerr << "benchmark ingress unexpectedly full\n";
            exitBenchmark(EXIT_FAILURE);
        }
    }

    const auto start = std::chrono::steady_clock::now();
    std::thread productionLoop([&sequencer]() { sequencer.run(); });
    productionLoop.detach();

    for (std::size_t index = 0; index < COMMAND_COUNT; ++index) {
        exchange::sequencer::sequenceMessage command{};
        while (!matching.pop(command)) {
            std::this_thread::yield();
        }
        if (command.globalSequenceNumber.value() != index + 1) {
            std::cerr << "benchmark observed non-FIFO command sequence\n";
            exitBenchmark(EXIT_FAILURE);
        }
    }

    const auto elapsed = std::chrono::steady_clock::now() - start;
    const auto elapsedMicroseconds = std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count();
    const double commandsPerSecond =
        static_cast<double>(COMMAND_COUNT) * 1'000'000.0 / static_cast<double>(elapsedMicroseconds);
    std::cout << "commands=" << COMMAND_COUNT << " elapsed_us=" << elapsedMicroseconds
              << " commands_per_second=" << commandsPerSecond << '\n';
    exitBenchmark(EXIT_SUCCESS);
}
