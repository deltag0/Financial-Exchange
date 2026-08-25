#include <atomic>
#include <chrono>
#include <gtest/gtest.h>
#include <iostream>
#include <thread>

#include "../../bus/include/bus.hpp"
#include "../../core/fix/include/fix_task.hpp"
#include "../../core/shared_queue/include/shared_queue.hpp"
#include "../../matching_engine/include/matching_engine.hpp"
#include "../../sequencer/include/sequencer.hpp"

// Pre-include STL headers so the throw(...) macro hack doesn't break them
#include <cstring>
#include <memory>
#include <vector>

#if __cplusplus >= 201703L
#define throw(...)
#endif

#include <quickfix/Fields.h>
#include <quickfix/Message.h>
#include <quickfix/Session.h>
#include <quickfix/fix44/NewOrderSingle.h>

#if __cplusplus >= 201703L
#undef throw
#endif

using namespace exchange;

namespace {
// Testable sequencer subclass to call run without infinite loop
class TestableSequencer : public sequencer::Sequencer {
public:
    using sequencer::Sequencer::Sequencer;
    bool processOnce() {
        return processNext();
    }
};

// Testable matching engine subclass
class TestableMatchingEngine : public matching_engine::MatchingEngine {
public:
    using matching_engine::MatchingEngine::MatchingEngine;
    void invokeDrain() {
        drainQueue(sequencerQueue, "Sequencer");
    }
};

FIX::Message makeFixNewOrder(const std::string& clientCommandId) {
    FIX::Message message;
    message.getHeader().setField(FIX::MsgType("D"));
    message.setField(FIX::ClOrdID(clientCommandId));
    message.setField(FIX::Symbol("SPY"));
    message.setField(FIX::Side(FIX::Side_BUY));
    message.setField(FIX::OrderQty(10));
    message.setField(FIX::Price(100.0));
    message.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    message.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));
    return message;
}

FIX::Message makeFixCancel(const std::string& clientCommandId, const std::string& orderId) {
    FIX::Message message;
    message.getHeader().setField(FIX::MsgType("F"));
    message.setField(FIX::ClOrdID(clientCommandId));
    message.setField(FIX::StringField(FIX::FIELD::OrderID, orderId));
    message.setField(FIX::OrigClOrdID("UNRELATED-PROTOCOL-CORRELATION"));
    message.setField(FIX::Symbol("SPY"));
    return message;
}
} // namespace

TEST(SequencerCancelNormalizationTest, ValidFixCancelGetsDistinctSequenceAndReachesMatching) {
    core::SharedQueue<sequencer::sequenceMessage> shardQueue(16);
    core::SharedQueue<sequencer::sequenceMessage> matchingQueue(16);
    std::vector<core::SharedQueue<sequencer::sequenceMessage>*> shardQueues{&shardQueue};
    core::Bus bus(16);
    const FIX::SessionID session("FIX.4.4", "CANCEL-CLIENT", "EXCHANGE");
    const core::fix::ClientIdentityResolver clientIdentityResolver({{session, domain::ClientId{500}}});
    core::task::FixTask fixTask(shardQueues, bus, clientIdentityResolver);
    TestableSequencer sequencer(shardQueues, &matchingQueue);
    matching_engine::BoundedCommandResultQueue resultQueue(4);
    TestableMatchingEngine matchingEngine(&matchingQueue, bus, resultQueue);

    fixTask.fromApp(makeFixNewOrder("NEW-1"), session);
    sequencer::sequenceMessage normalizedNew{};
    ASSERT_TRUE(fixTask.getFixMessageQueue()->pop(normalizedNew));
    fixTask.sendSequencerMessage(normalizedNew);
    ASSERT_TRUE(sequencer.processOnce());

    sequencer::sequenceMessage sequencedNew{};
    ASSERT_TRUE(matchingQueue.pop(sequencedNew));
    EXPECT_EQ(sequencedNew.globalSequenceNumber, domain::CommandSequence{1});
    EXPECT_EQ(sequencedNew.orderId, domain::OrderId{1});
    ASSERT_TRUE(matchingQueue.push(sequencedNew));
    matchingEngine.invokeDrain();
    matching_engine::ImmutableCommandResultBatch newResult;
    ASSERT_TRUE(resultQueue.tryPop(newResult));
    ASSERT_EQ(newResult->events().size(), 1u);
    ASSERT_NE(std::get_if<domain::OrderRested>(&newResult->events().front()), nullptr);

    fixTask.fromApp(makeFixCancel("INVALID-CANCEL", "0"), session);
    sequencer::sequenceMessage rejectedCancel{};
    EXPECT_FALSE(fixTask.getFixMessageQueue()->pop(rejectedCancel));
    EXPECT_FALSE(sequencer.processOnce());

    fixTask.fromApp(makeFixCancel("CANCEL-2", "1"), session);
    sequencer::sequenceMessage normalizedCancel{};
    ASSERT_TRUE(fixTask.getFixMessageQueue()->pop(normalizedCancel));
    EXPECT_EQ(normalizedCancel.globalSequenceNumber, domain::CommandSequence{});
    EXPECT_EQ(normalizedCancel.orderId, domain::OrderId{});
    EXPECT_EQ(normalizedCancel.targetOrderId, std::optional<domain::TargetOrderId>{domain::TargetOrderId{1}});
    fixTask.sendSequencerMessage(normalizedCancel);
    ASSERT_TRUE(sequencer.processOnce());

    sequencer::sequenceMessage sequencedCancel{};
    ASSERT_TRUE(matchingQueue.pop(sequencedCancel));
    EXPECT_EQ(sequencedCancel.globalSequenceNumber, domain::CommandSequence{2});
    EXPECT_NE(sequencedCancel.globalSequenceNumber, sequencedNew.globalSequenceNumber);
    EXPECT_EQ(sequencedCancel.orderId, domain::OrderId{});
    EXPECT_EQ(sequencedCancel.targetOrderId, normalizedCancel.targetOrderId);
    ASSERT_TRUE(matchingQueue.push(sequencedCancel));
    matchingEngine.invokeDrain();

    matching_engine::ImmutableCommandResultBatch cancelResult;
    ASSERT_TRUE(resultQueue.tryPop(cancelResult));
    EXPECT_EQ(cancelResult->commandSequence(), domain::CommandSequence{2});
    ASSERT_EQ(cancelResult->events().size(), 1u);
    const auto* cancelled = std::get_if<domain::OrderCancelled>(&cancelResult->events().front());
    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(cancelled->orderId, domain::OrderId{1});
    EXPECT_EQ(cancelled->cancelledQuantity, domain::Quantity{10});
    EXPECT_EQ(cancelled->reason, domain::CancelReason::CLIENT_REQUESTED);
}

TEST(SequencerClientIdentityTest, OwnershipUsesConfiguredClientIdDespiteChangedAndCollidingLegacyPorts) {
    core::SharedQueue<sequencer::sequenceMessage> shardQueue(16);
    core::SharedQueue<sequencer::sequenceMessage> matchingQueue(16);
    std::vector<core::SharedQueue<sequencer::sequenceMessage>*> shardQueues{&shardQueue};
    core::Bus bus(16);
    const FIX::SessionID primarySession("FIX.4.4", "EXCHANGE", "CLIENT-A");
    const FIX::SessionID alternateSession("FIX.4.4", "EXCHANGE", "CLIENT-A-ALT");
    const FIX::SessionID otherClientSession("FIX.4.4", "EXCHANGE", "CLIENT-B");
    const FIX::SessionID unknownSession("FIX.4.4", "EXCHANGE", "UNKNOWN");
    const core::fix::ClientIdentityResolver clientIdentityResolver({
        {primarySession, domain::ClientId{7001}},
        {alternateSession, domain::ClientId{7001}},
        {otherClientSession, domain::ClientId{8002}},
    });
    core::task::FixTask fixTask(shardQueues, bus, clientIdentityResolver);
    TestableSequencer sequencer(shardQueues, &matchingQueue);
    matching_engine::BoundedCommandResultQueue resultQueue(8);
    TestableMatchingEngine matchingEngine(&matchingQueue, bus, resultQueue);

    const auto normalizeAndSequence =
        [&](const FIX::Message& message, const FIX::SessionID& session,
            const std::uint64_t forcedLegacyPort) -> std::optional<sequencer::sequenceMessage> {
        fixTask.fromApp(message, session);
        sequencer::sequenceMessage normalized{};
        if (!fixTask.getFixMessageQueue()->pop(normalized)) {
            return std::nullopt;
        }
        normalized.port = forcedLegacyPort;
        fixTask.sendSequencerMessage(normalized);
        if (!sequencer.processOnce()) {
            return std::nullopt;
        }
        sequencer::sequenceMessage sequenced{};
        if (!matchingQueue.pop(sequenced)) {
            return std::nullopt;
        }
        return sequenced;
    };
    const auto match = [&](const sequencer::sequenceMessage& command) -> matching_engine::ImmutableCommandResultBatch {
        if (!matchingQueue.push(command)) {
            return {};
        }
        matchingEngine.invokeDrain();
        matching_engine::ImmutableCommandResultBatch result;
        if (!resultQueue.tryPop(result)) {
            return {};
        }
        return result;
    };

    const auto firstNew = normalizeAndSequence(makeFixNewOrder("NEW-1"), primarySession, 111);
    ASSERT_TRUE(firstNew.has_value());
    EXPECT_EQ(firstNew->globalSequenceNumber, domain::CommandSequence{1});
    EXPECT_EQ(firstNew->orderId, domain::OrderId{1});
    EXPECT_EQ(firstNew->clientId, domain::ClientId{7001});
    ASSERT_TRUE(match(*firstNew));

    const auto secondNew = normalizeAndSequence(makeFixNewOrder("NEW-2"), primarySession, 222);
    ASSERT_TRUE(secondNew.has_value());
    EXPECT_EQ(secondNew->globalSequenceNumber, domain::CommandSequence{2});
    EXPECT_EQ(secondNew->orderId, domain::OrderId{2});
    EXPECT_EQ(secondNew->clientId, domain::ClientId{7001});
    ASSERT_TRUE(match(*secondNew));

    fixTask.fromApp(makeFixCancel("UNKNOWN", "1"), unknownSession);
    sequencer::sequenceMessage rejectedIdentity{};
    EXPECT_FALSE(fixTask.getFixMessageQueue()->pop(rejectedIdentity));
    EXPECT_FALSE(sequencer.processOnce());
    EXPECT_TRUE(matchingQueue.empty());

    const auto alternateCancel = normalizeAndSequence(makeFixCancel("ALT-CANCEL", "1"), alternateSession, 333);
    ASSERT_TRUE(alternateCancel.has_value());
    EXPECT_EQ(alternateCancel->globalSequenceNumber, domain::CommandSequence{3});
    EXPECT_EQ(alternateCancel->orderId, domain::OrderId{});
    EXPECT_EQ(alternateCancel->targetOrderId, std::optional<domain::TargetOrderId>{domain::TargetOrderId{1}});
    EXPECT_EQ(alternateCancel->clientId, domain::ClientId{7001});
    EXPECT_NE(alternateCancel->port, firstNew->port);
    const matching_engine::ImmutableCommandResultBatch alternateResult = match(*alternateCancel);
    ASSERT_TRUE(alternateResult);
    ASSERT_EQ(alternateResult->events().size(), 1u);
    const auto* cancelled = std::get_if<domain::OrderCancelled>(&alternateResult->events().front());
    ASSERT_NE(cancelled, nullptr);
    EXPECT_EQ(cancelled->reason, domain::CancelReason::CLIENT_REQUESTED);

    const auto otherClientCancel =
        normalizeAndSequence(makeFixCancel("OTHER-CANCEL", "2"), otherClientSession, alternateCancel->port);
    ASSERT_TRUE(otherClientCancel.has_value());
    EXPECT_EQ(otherClientCancel->globalSequenceNumber, domain::CommandSequence{4});
    EXPECT_EQ(otherClientCancel->clientId, domain::ClientId{8002});
    EXPECT_EQ(otherClientCancel->port, alternateCancel->port);
    const matching_engine::ImmutableCommandResultBatch otherResult = match(*otherClientCancel);
    ASSERT_TRUE(otherResult);
    ASSERT_EQ(otherResult->events().size(), 1u);
    const auto* rejected = std::get_if<domain::CommandRejected>(&otherResult->events().front());
    ASSERT_NE(rejected, nullptr);
    EXPECT_EQ(rejected->reason, domain::CommandRejectionReason::NOT_OWNER);
}

TEST(E2EThroughputTest, SequencerToMatchingEngineThroughput) {
    // Test: Measure throughput from Sequencer → MatchingEngine (excluding FIX parsing)
    const int num_shards = 4;
    const int queue_size = 50000; // Must be < 65535 for boost lockfree

    std::vector<core::SharedQueue<sequencer::sequenceMessage>*> sequencer_queues;
    for (int i = 0; i < num_shards; ++i) {
        sequencer_queues.push_back(new core::SharedQueue<sequencer::sequenceMessage>(queue_size));
    }

    core::SharedQueue<sequencer::sequenceMessage> matching_engine_queue(queue_size);
    core::Bus multicast_bus(131072);
    matching_engine::BoundedCommandResultQueue command_result_queue(queue_size);

    // Create pipeline components
    auto sequencer = std::make_unique<sequencer::Sequencer>(sequencer_queues, &matching_engine_queue);
    auto matching_engine =
        std::make_unique<matching_engine::MatchingEngine>(&matching_engine_queue, multicast_bus, command_result_queue);

    std::atomic<bool> stop_sequencer{false};
    std::atomic<bool> stop_matching{false};
    std::atomic<int> messages_sequenced{0};
    std::atomic<int> messages_matched{0};

    // Sequencer thread: drains input queues and forwards to matching engine
    std::thread sequencer_thread([&]() {
        while (!stop_sequencer) {
            for (int shard = 0; shard < num_shards; ++shard) {
                if (sequencer_queues[shard]->empty()) continue;

                sequencer::sequenceMessage msg{};
                if (sequencer_queues[shard]->pop(msg)) {
                    msg.globalSequenceNumber = sequencer->getNextGlobalSequenceNumber(msg);
                    msg.topicSequenceNumber = sequencer->getNextTopicSequenceNumber(msg);

                    if (matching_engine_queue.push(msg)) {
                        messages_sequenced.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        // Queue full, try again
                        sequencer_queues[shard]->push(msg);
                    }
                }
            }
            std::this_thread::yield();
        }
    });

    // Matching engine thread: drains sequencer queue
    std::thread matching_thread([&]() {
        while (!stop_matching || !matching_engine_queue.empty()) {
            sequencer::sequenceMessage msg{};
            if (matching_engine_queue.pop(msg)) {
                matching_engine->send(msg);
                messages_matched.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Client simulation: spam messages for 1 second
    auto start = std::chrono::high_resolution_clock::now();
    auto end_time = start + std::chrono::seconds(1);
    int client_messages_sent = 0;

    while (std::chrono::high_resolution_clock::now() < end_time) {
        sequencer::sequenceMessage msg{};
        msg.id = client_messages_sent;
        msg.shard_id = client_messages_sent % num_shards;
        msg.price = domain::Price{static_cast<std::uint64_t>((client_messages_sent % 100) + 1)};
        msg.quantity = domain::Quantity{10};
        msg.port = (client_messages_sent % 100);
        msg.topic = msg.port;
        strcpy(msg.symbol, "SPY");
        msg.type = sequencer::orderType::BUY;

        if (sequencer_queues[msg.shard_id]->push(msg)) {
            client_messages_sent++;
        } else {
            // Queue full, back off briefly
            std::this_thread::yield();
        }
    }

    auto send_end = std::chrono::high_resolution_clock::now();
    auto send_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(send_end - start).count();

    // Wait for pipeline to drain (up to 500ms)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop_sequencer = true;
    sequencer_thread.join();

    // Give matching engine time to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_matching = true;
    matching_thread.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto total_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    int seq_count = messages_sequenced.load();
    int match_count = messages_matched.load();
    double client_throughput = (client_messages_sent * 1000.0) / send_duration_ms;
    double total_throughput = (match_count * 1000.0) / total_duration_ms;

    std::cout << "\n========== E2E Throughput Test Results ==========" << std::endl;
    std::cout << "Client send phase: " << send_duration_ms << " ms" << std::endl;
    std::cout << "  Messages sent by clients: " << client_messages_sent << std::endl;
    std::cout << "  Client throughput: " << client_throughput << " msg/sec" << std::endl;
    std::cout << "\nPipeline processing:" << std::endl;
    std::cout << "  Messages sequenced: " << seq_count << std::endl;
    std::cout << "  Messages matched: " << match_count << std::endl;
    std::cout << "  Total time: " << total_duration_ms << " ms" << std::endl;
    std::cout << "  End-to-end throughput: " << total_throughput << " msg/sec" << std::endl;
    std::cout << "  Avg time per message: " << (total_duration_ms * 1000.0 / match_count) << " us" << std::endl;
    std::cout << "===============================================\n" << std::endl;

    // Cleanup
    for (auto q : sequencer_queues) {
        delete q;
    }

    // Sanity checks
    EXPECT_GT(client_messages_sent, 0);
    EXPECT_GT(seq_count, 0);
    EXPECT_GT(match_count, 0);
    EXPECT_EQ(seq_count, match_count);
}

TEST(E2EThroughputTest, FullPipelineWithFixParsing) {
    // Test: Measure throughput from FixTask (FIX parsing) → Sequencer → MatchingEngine
    const int num_shards = 4;
    const int queue_size = 50000;

    // Setup pipeline queues
    std::vector<core::SharedQueue<sequencer::sequenceMessage>*> sequencer_queues;
    for (int i = 0; i < num_shards; ++i) {
        sequencer_queues.push_back(new core::SharedQueue<sequencer::sequenceMessage>(queue_size));
    }

    core::SharedQueue<sequencer::sequenceMessage> matching_engine_queue(queue_size);
    core::Bus multicast_bus(131072);
    matching_engine::BoundedCommandResultQueue command_result_queue(queue_size);

    // Create FixTask
    const FIX::SessionID sessionID("FIX.4.4", "SENDER", "TARGET");
    const core::fix::ClientIdentityResolver clientIdentityResolver({{sessionID, domain::ClientId{600}}});
    core::task::FixTask fix_task(sequencer_queues, multicast_bus, clientIdentityResolver);
    auto fix_queue = fix_task.getFixMessageQueue();

    // Create sequencer and matching engine
    auto sequencer = std::make_unique<sequencer::Sequencer>(sequencer_queues, &matching_engine_queue);
    auto matching_engine =
        std::make_unique<matching_engine::MatchingEngine>(&matching_engine_queue, multicast_bus, command_result_queue);

    std::atomic<bool> stop_fix{false};
    std::atomic<bool> stop_sequencer{false};
    std::atomic<bool> stop_matching{false};
    std::atomic<int> messages_from_fix{0};
    std::atomic<int> messages_sequenced{0};
    std::atomic<int> messages_matched{0};

    // FixTask processing thread: drain fix queue and push to sequencer queues
    std::thread fix_thread([&]() {
        while (!stop_fix || !fix_queue->empty()) {
            sequencer::sequenceMessage msg;
            if (fix_queue->pop(msg)) {
                messages_from_fix.fetch_add(1, std::memory_order_relaxed);
                int shard = msg.shard_id % num_shards;
                while (!sequencer_queues[shard]->push(msg) && !stop_fix) {
                    std::this_thread::yield();
                }
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Sequencer thread
    std::thread sequencer_thread([&]() {
        while (!stop_sequencer) {
            for (int shard = 0; shard < num_shards; ++shard) {
                if (sequencer_queues[shard]->empty()) continue;

                sequencer::sequenceMessage msg{};
                if (sequencer_queues[shard]->pop(msg)) {
                    msg.globalSequenceNumber = sequencer->getNextGlobalSequenceNumber(msg);
                    msg.topicSequenceNumber = sequencer->getNextTopicSequenceNumber(msg);

                    if (matching_engine_queue.push(msg)) {
                        messages_sequenced.fetch_add(1, std::memory_order_relaxed);
                    } else {
                        sequencer_queues[shard]->push(msg);
                    }
                }
            }
            std::this_thread::yield();
        }
    });

    // Matching engine thread
    std::thread matching_thread([&]() {
        while (!stop_matching || !matching_engine_queue.empty()) {
            sequencer::sequenceMessage msg{};
            if (matching_engine_queue.pop(msg)) {
                matching_engine->send(msg);
                messages_matched.fetch_add(1, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    // Simulate FIX clients sending actual FIX messages for 1 second
    auto start = std::chrono::high_resolution_clock::now();
    auto end_time = start + std::chrono::seconds(1);
    int fix_messages_injected = 0;

    while (std::chrono::high_resolution_clock::now() < end_time) {
        // Create a FIX NewOrderSingle message
        FIX44::NewOrderSingle order;

        // Required fields for order
        order.set(FIX::ClOrdID(std::to_string(fix_messages_injected)));
        order.set(FIX::Symbol("SPY"));
        order.set(FIX::Side(FIX::Side_BUY)); // Buy order
        order.set(FIX::TransactTime());
        order.set(FIX::OrderQty(10.0));
        order.set(FIX::Price((fix_messages_injected % 100) + 1.0));
        order.set(FIX::OrdType(FIX::OrdType_LIMIT));
        order.set(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));

        // Send through FixTask's fromApp() to parse into sequenceMessage
        try {
            fix_task.fromApp(order, sessionID);
            fix_messages_injected++;
        } catch (...) {
            std::this_thread::yield();
        }
    }

    auto send_end = std::chrono::high_resolution_clock::now();
    auto send_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(send_end - start).count();

    // Wait for pipeline to drain
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    stop_fix = true;
    fix_thread.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_sequencer = true;
    sequencer_thread.join();

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    stop_matching = true;
    matching_thread.join();

    auto end = std::chrono::high_resolution_clock::now();
    auto total_duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    int fix_count = messages_from_fix.load();
    int seq_count = messages_sequenced.load();
    int match_count = messages_matched.load();

    std::cout << "\n========== Full Pipeline (FIX + Sequencer + MatchingEngine) ==========" << std::endl;
    std::cout << "FIX message injection: " << send_duration_ms << " ms" << std::endl;
    std::cout << "  Messages injected: " << fix_messages_injected << std::endl;
    std::cout << "  Injection throughput: " << (fix_messages_injected * 1000.0 / send_duration_ms) << " msg/sec"
              << std::endl;
    std::cout << "\nFull pipeline processing:" << std::endl;
    std::cout << "  Messages from FIX queue: " << fix_count << std::endl;
    std::cout << "  Messages sequenced: " << seq_count << std::endl;
    std::cout << "  Messages matched: " << match_count << std::endl;
    std::cout << "  Total time: " << total_duration_ms << " ms" << std::endl;
    std::cout << "  End-to-end throughput: " << (match_count * 1000.0 / total_duration_ms) << " msg/sec" << std::endl;
    std::cout << "  Avg time per message: " << (total_duration_ms * 1000.0 / match_count) << " us" << std::endl;
    std::cout << "====================================================================\n" << std::endl;

    // Cleanup
    for (auto q : sequencer_queues) {
        delete q;
    }

    // Sanity checks
    EXPECT_GT(fix_messages_injected, 0);
    EXPECT_GT(fix_count, 0);
    EXPECT_GT(seq_count, 0);
    EXPECT_GT(match_count, 0);
}
