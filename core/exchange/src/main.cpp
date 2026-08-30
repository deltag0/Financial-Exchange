#include <admission_completion_consumer.hpp>
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

#define BUS_SIZE 16384
#define COMMAND_ADMISSION_CAPACITY 100000
#define SEQUENCING_INGRESS_QUEUE_SIZE 1000
#define MATCHING_ENGINE_QUEUE_SIZE 1000
#define COMMAND_RESULT_QUEUE_SIZE 1000

int main() {
    std::cout << "Starting Exchange FIX Acceptor with one process-local sequencing boundary..." << std::endl;
    try {
        FIX::SessionSettings settings("/app/exchange.cfg");
        const exchange::core::fix::ClientIdentityResolver clientIdentityResolver(settings);

        exchange::core::SharedQueue<exchange::sequencer::sequenceMessage> sequencingIngressQueue(
            SEQUENCING_INGRESS_QUEUE_SIZE);
        exchange::core::SharedQueue<exchange::sequencer::sequenceMessage> matchingEngineQueue(
            MATCHING_ENGINE_QUEUE_SIZE);

        exchange::core::Bus multicastBus(BUS_SIZE);
        exchange::core::admission::CommandAdmissionIndex commandAdmissionIndex(COMMAND_ADMISSION_CAPACITY);
        exchange::matching_engine::BoundedCommandResultQueue commandResultQueue(COMMAND_RESULT_QUEUE_SIZE);
        exchange::core::admission::AdmissionCompletionConsumer admissionCompletionConsumer(commandResultQueue,
                                                                                           commandAdmissionIndex);

        exchange::core::task::FixTask application(sequencingIngressQueue, multicastBus, clientIdentityResolver,
                                                  commandAdmissionIndex);

        exchange::sequencer::Sequencer sequencer(sequencingIngressQueue, matchingEngineQueue, commandAdmissionIndex);
        exchange::matching_engine::MatchingEngine matching_engine(&matchingEngineQueue, multicastBus,
                                                                  commandResultQueue);

        // QuickFIX session threads publish admitted commands to FixTask's bounded staging queue.
        // This worker forwards them in staging FIFO order to sequencingIngressQueue.
        std::thread fix_task_thread([&application]() { application.run(); });

        std::thread matching_engine_thread([&matching_engine]() { matching_engine.run(); });
        std::thread admission_completion_thread(
            [&admissionCompletionConsumer]() { admissionCompletionConsumer.run(); });
        std::thread sequencer_thread([&sequencer]() { sequencer.run(); });

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
        if (sequencer_thread.joinable()) {
            sequencer_thread.join();
        }
        if (matching_engine_thread.joinable()) {
            matching_engine_thread.join();
        }
        if (admission_completion_thread.joinable()) {
            admission_completion_thread.join();
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
