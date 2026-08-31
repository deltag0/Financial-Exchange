#include "fix_parser.hpp"
#include "fix_task.hpp"
#include "fix_test_identities.hpp"
#include <gtest/gtest.h>

#include <atomic>
#include <barrier>
#include <thread>
#include <vector>

using namespace exchange::core::task;
using namespace exchange::sequencer;

namespace {

FIX::Message makeNewOrder(const std::string& clientCommandId) {
    FIX::Message message;
    message.getHeader().setField(FIX::MsgType("D"));
    message.setField(FIX::ClOrdID(clientCommandId));
    message.setField(FIX::Symbol("SPY"));
    message.setField(FIX::Side(FIX::Side_BUY));
    message.setField(FIX::OrderQty(5));
    message.setField(FIX::Price(1.23));
    message.setField(FIX::OrdType(FIX::OrdType_LIMIT));
    message.setField(FIX::TimeInForce(FIX::TimeInForce_GOOD_TILL_CANCEL));
    return message;
}

} // namespace

TEST(FixTaskTest, FromAppStagesThenWorkerForwardsToSequencingIngress) {
    exchange::core::SharedQueue<sequenceMessage> sequencingIngressQueue(16);
    exchange::core::Bus bus(8);
    exchange::core::admission::CommandAdmissionIndex admissionIndex(8);

    FixTask fixTask(sequencingIngressQueue, bus, exchange::core::fix::test::clientIdentityResolver(), admissionIndex);

    // Build a simple NewOrderSingle
    FIX::Message msg = makeNewOrder("ORD1");

    FIX::SessionID sid("FIX.4.4", "SENDER", "TARGET");

    // Call fromApp which should parse and push into internal queue
    fixTask.fromApp(msg, sid);
    EXPECT_TRUE(sequencingIngressQueue.empty());
    ASSERT_TRUE(fixTask.processNextStagedCommand());

    sequenceMessage out{};
    bool popped = sequencingIngressQueue.pop(out);
    EXPECT_TRUE(popped);
    EXPECT_EQ(out.quantity.value(), 5);
    EXPECT_EQ(out.type, orderType::BUY);
    EXPECT_STREQ(out.symbol, "SPY");
}

TEST(FixTaskTest, FullSequencingIngressRetainsOnePendingAndForwardsExactlyOnceInOrder) {
    exchange::core::SharedQueue<sequenceMessage> sequencingIngressQueue(1);
    exchange::core::Bus bus(8);
    exchange::core::admission::CommandAdmissionIndex admissionIndex(8);
    FixTask fixTask(sequencingIngressQueue, bus, exchange::core::fix::test::clientIdentityResolver(), admissionIndex,
                    1);
    const FIX::SessionID session("FIX.4.4", "SENDER", "TARGET");

    sequenceMessage blocker{};
    blocker.globalSequenceNumber = exchange::domain::CommandSequence{999};
    ASSERT_TRUE(sequencingIngressQueue.push(blocker));
    fixTask.fromApp(makeNewOrder("FIRST"), session);
    EXPECT_FALSE(fixTask.processNextStagedCommand());
    ASSERT_TRUE(fixTask.hasPendingStagedCommand());
    ASSERT_TRUE(fixTask.pendingStagedCommand()->clientCommandId.has_value());
    EXPECT_EQ(fixTask.pendingStagedCommand()->clientCommandId->value(), "FIRST");

    fixTask.fromApp(makeNewOrder("SECOND"), session);
    EXPECT_FALSE(fixTask.processNextStagedCommand());
    ASSERT_TRUE(fixTask.hasPendingStagedCommand());
    ASSERT_TRUE(fixTask.pendingStagedCommand()->clientCommandId.has_value());
    EXPECT_EQ(fixTask.pendingStagedCommand()->clientCommandId->value(), "FIRST");
    EXPECT_FALSE(fixTask.stagingQueueEmpty());
    EXPECT_EQ(admissionIndex.statistics().firstSubmissions, 2u);

    sequenceMessage removedBlocker{};
    ASSERT_TRUE(sequencingIngressQueue.pop(removedBlocker));
    ASSERT_EQ(removedBlocker.globalSequenceNumber, exchange::domain::CommandSequence{999});
    ASSERT_TRUE(fixTask.processNextStagedCommand());
    EXPECT_FALSE(fixTask.hasPendingStagedCommand());

    sequenceMessage first{};
    ASSERT_TRUE(sequencingIngressQueue.pop(first));
    ASSERT_TRUE(first.clientCommandId.has_value());
    EXPECT_EQ(first.clientCommandId->value(), "FIRST");

    ASSERT_TRUE(fixTask.processNextStagedCommand());
    sequenceMessage second{};
    ASSERT_TRUE(sequencingIngressQueue.pop(second));
    ASSERT_TRUE(second.clientCommandId.has_value());
    EXPECT_EQ(second.clientCommandId->value(), "SECOND");
    EXPECT_FALSE(fixTask.processNextStagedCommand());
    EXPECT_TRUE(sequencingIngressQueue.empty());
}

TEST(FixTaskTest, FullStagingQueueAbandonsReservationAndAllowsIdenticalRetry) {
    exchange::core::SharedQueue<sequenceMessage> sequencingIngressQueue(1);
    exchange::core::Bus bus(8);
    exchange::core::admission::CommandAdmissionIndex admissionIndex(8);
    FixTask fixTask(sequencingIngressQueue, bus, exchange::core::fix::test::clientIdentityResolver(), admissionIndex,
                    1);
    const FIX::SessionID session("FIX.4.4", "SENDER", "TARGET");

    fixTask.fromApp(makeNewOrder("FIRST"), session);
    fixTask.fromApp(makeNewOrder("RETRY"), session);
    EXPECT_EQ(admissionIndex.size(), 1u);
    EXPECT_EQ(admissionIndex.statistics().firstSubmissions, 2u);
    EXPECT_EQ(fixTask.statistics().stagingQueueSaturations, 1u);
    EXPECT_EQ(fixTask.statistics().reservationAbandonFailures, 0u);

    ASSERT_TRUE(fixTask.processNextStagedCommand());
    sequenceMessage first{};
    ASSERT_TRUE(sequencingIngressQueue.pop(first));
    ASSERT_TRUE(first.clientCommandId.has_value());
    EXPECT_EQ(first.clientCommandId->value(), "FIRST");

    fixTask.fromApp(makeNewOrder("RETRY"), session);
    EXPECT_EQ(admissionIndex.size(), 2u);
    EXPECT_EQ(admissionIndex.statistics().firstSubmissions, 3u);
    ASSERT_TRUE(fixTask.processNextStagedCommand());

    sequenceMessage retry{};
    ASSERT_TRUE(sequencingIngressQueue.pop(retry));
    ASSERT_TRUE(retry.clientCommandId.has_value());
    EXPECT_EQ(retry.clientCommandId->value(), "RETRY");
    EXPECT_TRUE(sequencingIngressQueue.empty());
}

TEST(FixTaskTest, ConcurrentNormalizationRejectionsHaveAnExactMonotonicDiagnosticCount) {
    constexpr std::size_t producerCount = 8;
    constexpr std::size_t rejectionsPerProducer = 64;
    exchange::core::SharedQueue<sequenceMessage> sequencingIngressQueue(1);
    exchange::core::Bus bus(8);
    exchange::core::admission::CommandAdmissionIndex admissionIndex(8);
    FixTask fixTask(sequencingIngressQueue, bus, exchange::core::fix::test::clientIdentityResolver(), admissionIndex,
                    1);
    const FIX::SessionID session("FIX.4.4", "SENDER", "TARGET");
    std::barrier start(static_cast<std::ptrdiff_t>(producerCount + 1));
    std::atomic<std::size_t> completedProducers{0};
    std::vector<std::thread> producers;
    producers.reserve(producerCount);

    for (std::size_t producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&, producer]() {
            FIX::Message invalid = makeNewOrder("INVALID-" + std::to_string(producer));
            invalid.removeField(FIX::FIELD::Symbol);
            start.arrive_and_wait();
            for (std::size_t attempt = 0; attempt < rejectionsPerProducer; ++attempt) {
                fixTask.fromApp(invalid, session);
            }
            completedProducers.fetch_add(1, std::memory_order_release);
        });
    }

    start.arrive_and_wait();
    std::uint64_t previous = 0;
    bool monotonic = true;
    while (completedProducers.load(std::memory_order_acquire) != producerCount) {
        const std::uint64_t current = fixTask.statistics().normalizationRejections;
        monotonic = monotonic && current >= previous;
        previous = current;
        std::this_thread::yield();
    }
    for (std::thread& producer : producers) {
        producer.join();
    }

    EXPECT_TRUE(monotonic);
    EXPECT_EQ(fixTask.statistics().normalizationRejections, producerCount * rejectionsPerProducer);
    EXPECT_EQ(fixTask.statistics().internalFailures, 0u);
    EXPECT_EQ(admissionIndex.size(), 0u);
    EXPECT_TRUE(fixTask.stagingQueueEmpty());
}

TEST(FixTaskTest, ConcurrentStagingSaturationCountsEachRejectedHandoffAndAbandonsItsReservation) {
    constexpr std::size_t producerCount = 8;
    exchange::core::SharedQueue<sequenceMessage> sequencingIngressQueue(1);
    exchange::core::Bus bus(8);
    exchange::core::admission::CommandAdmissionIndex admissionIndex(producerCount);
    FixTask fixTask(sequencingIngressQueue, bus, exchange::core::fix::test::clientIdentityResolver(), admissionIndex,
                    1);
    const FIX::SessionID session("FIX.4.4", "SENDER", "TARGET");
    std::barrier start(static_cast<std::ptrdiff_t>(producerCount));
    std::vector<std::thread> producers;
    producers.reserve(producerCount);

    for (std::size_t producer = 0; producer < producerCount; ++producer) {
        producers.emplace_back([&, producer]() {
            start.arrive_and_wait();
            fixTask.fromApp(makeNewOrder("CONCURRENT-" + std::to_string(producer)), session);
        });
    }
    for (std::thread& producer : producers) {
        producer.join();
    }

    const FixTaskStatistics statistics = fixTask.statistics();
    EXPECT_EQ(statistics.normalizationRejections, 0u);
    EXPECT_EQ(statistics.internalFailures, 0u);
    EXPECT_EQ(statistics.stagingQueueSaturations, producerCount - 1);
    EXPECT_EQ(statistics.reservationAbandonFailures, 0u);
    EXPECT_EQ(admissionIndex.statistics().firstSubmissions, producerCount);
    EXPECT_EQ(admissionIndex.size(), 1u);
}
