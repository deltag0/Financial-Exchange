#include <fix_task.hpp>
#include <matching_engine.hpp>
#include <shared_queue.hpp>

#include "bus.hpp"
#include "sequencer.hpp"

#include <chrono>
#include <iostream>
#include <thread>

#if __cplusplus >= 201703L
#define throw(...)
#endif

#include <quickfix/FileLog.h>
#include <quickfix/FileStore.h>
#include <quickfix/SessionSettings.h>
#include <quickfix/SocketAcceptor.h>

#if __cplusplus >= 201703L
#undef throw
#endif

#define NUM_SHARDS 4
#define BUS_SIZE 16384

int main() {
    std::cout << "Starting Exchange FIX Acceptor with " << NUM_SHARDS << " Sequencer Shards..."
              << std::endl;
    try {
        FIX::SessionSettings settings("/app/exchange.cfg");

        std::vector<
            std::unique_ptr<exchange::core::SharedQueue<exchange::sequencer::sequenceMessage>>>
            shard_queues;
        std::vector<exchange::core::SharedQueue<exchange::sequencer::sequenceMessage> *>
            shard_queue_ptrs;
        auto matching_engine_queue =
            std::make_unique<exchange::core::SharedQueue<exchange::sequencer::sequenceMessage>>(
                1000);

        for (int i = 0; i < NUM_SHARDS; ++i) {
            shard_queues.push_back(
                std::make_unique<exchange::core::SharedQueue<exchange::sequencer::sequenceMessage>>(
                    1000));
            shard_queue_ptrs.push_back(shard_queues.back().get());
        }

        exchange::core::Bus<exchange::sequencer::sequenceMessage> multicastBus(BUS_SIZE);

        // Initialize FIX Application with all shards
        exchange::core::task::FixTask application(shard_queue_ptrs, multicastBus);

        exchange::matching_engine::MatchingEngine matching_engine(matching_engine_queue.get(),
                                                                  multicastBus);

        // FixTask now owns the continuous drain loop for FIX-to-sequencer traffic.
        // Keep it running on its own thread so the acceptor can keep handling ports.
        std::thread fix_task_thread([&application]() { application.run(); });

        std::thread matching_engine_thread([&matching_engine]() { matching_engine.run(); });

        // Start 4 Sequencer threads
        std::vector<std::unique_ptr<exchange::sequencer::Sequencer>> sequencers;
        std::vector<std::thread> sequencer_threads;

        for (int i = 0; i < NUM_SHARDS; ++i) {
            // Each sequencer gets its own shard queue (passed as a vector of size 1)
            std::vector<exchange::core::SharedQueue<exchange::sequencer::sequenceMessage> *>
                single_shard = {shard_queue_ptrs[i]};
            sequencers.push_back(std::make_unique<exchange::sequencer::Sequencer>(
                std::move(single_shard), matching_engine_queue.get()));

            sequencer_threads.emplace_back([&seq = *sequencers.back()]() { seq.run(); });
            std::cout << "[Main] Launched Sequencer Shard " << i << std::endl;
        }

        FIX::FileStoreFactory storeFactory(settings);
        FIX::FileLogFactory logFactory(settings);
        FIX::SocketAcceptor acceptor(application, storeFactory, settings, logFactory);

        acceptor.start();
        std::cout << "Acceptor successfully started! Listening on port 5001..." << std::endl;

        // Keep main thread alive
        while (true) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }

        if (fix_task_thread.joinable()) {
            fix_task_thread.join();
        }
        acceptor.stop();
        for (auto &t : sequencer_threads) {
            if (t.joinable())
                t.join();
        }
        if (matching_engine_thread.joinable()) {
            matching_engine_thread.join();
        }

    } catch (std::exception &e) {
        std::cerr << "Exception initializing Acceptor: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown exception starting Acceptor" << std::endl;
        return 1;
    }

    return 0;
}
