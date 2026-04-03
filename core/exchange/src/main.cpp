#include <shared_queue.hpp>
#include <fix_task.hpp>

#include <thread>
#include <chrono>
#include <iostream>
#include "sequencer.hpp"

#if __cplusplus >= 201703L
#define throw(...)
#endif

#include <quickfix/SocketAcceptor.h>
#include <quickfix/FileStore.h>
#include <quickfix/FileLog.h>
#include <quickfix/SessionSettings.h>

#if __cplusplus >= 201703L
#undef throw
#endif

#define NUM_SHARDS 4

/*

Trade for aapl comes in

added to sequencer

if 1 sequencer -> easy to assign sequence number, what we'll do

with multiple sequencers, but the tickers sharded, it's still easy to assign the sequence number

but then, for the shared queues, we're going to need multiple of them



*/

int main() {
    std::cout << "Starting Exchange FIX Acceptor with " << NUM_SHARDS << " Sequencer Shards..." << std::endl;
    try {
        FIX::SessionSettings settings("/app/exchange.cfg");

        std::vector<std::unique_ptr<exchange::core::SharedQueue<exchange::sequencer::sequenceMessage>>> shard_queues;
        std::vector<exchange::core::SharedQueue<exchange::sequencer::sequenceMessage>*> shard_queue_ptrs;

        for (int i = 0; i < NUM_SHARDS; ++i) {
            shard_queues.push_back(
                std::make_unique<exchange::core::SharedQueue<exchange::sequencer::sequenceMessage>>(1000));
            shard_queue_ptrs.push_back(shard_queues.back().get());
        }

        // Initialize FIX Application with all shards
        exchange::core::task::FixTask application(shard_queue_ptrs);

        // Start 4 Sequencer threads
        std::vector<std::unique_ptr<exchange::sequencer::Sequencer>> sequencers;
        std::vector<std::thread> sequencer_threads;

        for (int i = 0; i < NUM_SHARDS; ++i) {
            // Each sequencer gets its own shard queue (passed as a vector of size 1)
            std::vector<exchange::core::SharedQueue<exchange::sequencer::sequenceMessage>*> single_shard = {
                shard_queue_ptrs[i]};
            sequencers.push_back(std::make_unique<exchange::sequencer::Sequencer>(single_shard));

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

        acceptor.stop();
        for (auto& t : sequencer_threads) {
            if (t.joinable()) t.join();
        }

    } catch (std::exception& e) {
        std::cerr << "Exception initializing Acceptor: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "Unknown exception starting Acceptor" << std::endl;
        return 1;
    }

    return 0;
}
