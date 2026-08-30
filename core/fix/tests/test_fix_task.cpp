#include "fix_parser.hpp"
#include "fix_task.hpp"
#include "fix_test_identities.hpp"
#include <gtest/gtest.h>

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
    blocker.id = 999;
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
    ASSERT_EQ(removedBlocker.id, 999u);
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
