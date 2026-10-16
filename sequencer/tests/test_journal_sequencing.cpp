#include "sequencer.hpp"
#include "../../core/admission/tests/admission_test_access.hpp"

#include "recovered_run.hpp"
#include "run_journal_reader.hpp"
#include "run_journal_writer_internal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <unistd.h>
#include <vector>

namespace exchange::sequencer {
namespace {

constexpr domain::ExchangeRunId RUN_ID{42};

class TestableSequencer final : public Sequencer {
public:
    using Sequencer::Sequencer;
    bool step() {
        return processNext();
    }
    bool drain() {
        return drainAvailable();
    }
};

class JournalSequencerTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::array<char, 64> buffer{};
        constexpr char TEMPLATE[] = "/tmp/exchange-journal-sequencer-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), buffer.begin());
        const char* created = ::mkdtemp(buffer.data());
        ASSERT_NE(created, nullptr);
        directory_ = created;
    }
    void TearDown() override {
        if (!directory_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(directory_, error);
            EXPECT_FALSE(error);
        }
    }
    std::filesystem::path path() const {
        return directory_ / "run-42.fxjr";
    }

private:
    std::filesystem::path directory_;
};

storage::RunHeaderV1 header() {
    return {
        .exchangeRunId = RUN_ID,
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = 10,
        .maxRunJournalBytes = 64 * 1024,
        .instruments = {{domain::InstrumentId{1}, 1},
                        {domain::InstrumentId{200}, std::numeric_limits<std::uint32_t>::max()}},
    };
}

sequenceMessage newOrder(const std::string_view id = "NEW") {
    sequenceMessage message{};
    message.clientId = domain::ClientId{11};
    message.clientCommandId = domain::ClientCommandId{id};
    message.instrumentId = domain::InstrumentId{1};
    message.configurationVersion = 1;
    message.type = orderType::BUY;
    message.price = domain::Price{1'234};
    message.quantity = domain::Quantity{50};
    message.tif = core::task::TimeInForce::GTC;
    return message;
}

sequenceMessage cancel(const std::string_view id = "CANCEL") {
    auto message = newOrder(id);
    message.type = orderType::CANCEL;
    message.price = {};
    message.quantity = {};
    message.targetOrderId = domain::TargetOrderId{1};
    return message;
}

matching_engine::ImmutableCommandResultBatch completionProbe(const sequenceMessage& message,
                                                             const std::uint64_t sequence) {
    // Exercise admission's existing state check without executing matching before durability.
    return std::make_shared<const matching_engine::CommandResultBatch>(
        domain::CommandResultCorrelation{message.clientId, *message.clientCommandId, domain::CommandSequence{sequence},
                                         RUN_ID},
        matching_engine::ProcessingResult::APPLIED, std::vector<domain::BusinessEvent>{});
}

struct AppendScript final {
    int writes{0};
    int syncs{0};
    int failWrite{0};
    bool zeroWrite{false};
    bool failSync{false};
    bool interruptWrite{false};
    bool interruptSync{false};
    std::size_t maximumWrite{std::numeric_limits<std::size_t>::max()};
    core::SharedQueue<sequenceMessage>* matching{nullptr};
    core::admission::CommandAdmissionIndex* admission{nullptr};
    storage::RunJournalWriterV1* writer{nullptr};
    matching_engine::ImmutableCommandResultBatch probe{};
    std::uint64_t expectedCount{0};
    std::uint64_t expectedBytes{0};
};

ssize_t scriptedWrite(void* context, const int descriptor, const void* data, const std::size_t size) noexcept {
    auto& script = *static_cast<AppendScript*>(context);
    ++script.writes;
    if (script.interruptWrite && script.writes == 1) {
        errno = EINTR;
        return -1;
    }
    if (script.writes == script.failWrite) {
        if (script.zeroWrite) {
            return 0;
        }
        errno = ENOSPC;
        return -1;
    }
    return ::write(descriptor, data, std::min(size, script.maximumWrite));
}

int scriptedSync(void* context, const int descriptor) noexcept {
    auto& script = *static_cast<AppendScript*>(context);
    ++script.syncs;
    if (script.matching != nullptr) {
        EXPECT_TRUE(script.matching->empty());
        EXPECT_EQ(script.admission->complete(script.probe), core::admission::CompletionStatus::WRONG_STATE);
        EXPECT_EQ(script.writer->committedCommandCount(), script.expectedCount);
        EXPECT_EQ(script.writer->committedByteCount(), script.expectedBytes);
        EXPECT_EQ(script.writer->nextCommandSequence(), domain::CommandSequence{script.expectedCount + 1});
    }
    if (script.interruptSync && script.syncs == 1) {
        errno = EINTR;
        return -1;
    }
    if (script.failSync) {
        errno = EIO;
        return -1;
    }
    return ::fdatasync(descriptor);
}

TEST_F(JournalSequencerTest, NewOrderAndCancelBindAndReachMatchingOnlyAfterDurableAppend) {
    core::SharedQueue<sequenceMessage> ingress(4);
    core::SharedQueue<sequenceMessage> matching(4);
    core::admission::CommandAdmissionIndex admission(10, RUN_ID);
    core::admission::CommandAdmissionIndexTestAccess::setAdmissionOpen(admission, true);
    AppendScript script{.matching = &matching, .admission = &admission};
    std::unique_ptr<storage::RunJournalWriterV1> writer;
    ASSERT_EQ(storage::detail::createRunJournalWriterV1WithHooks(path(), header(),
                                                                 {&script, scriptedWrite, scriptedSync}, writer)
                  .outcome,
              storage::RunJournalCreateOutcome::CREATED);
    script.writer = writer.get();
    TestableSequencer sequencer(ingress, matching, admission, *writer);
    matching_engine::MatchingState state(RUN_ID);
    auto buy = newOrder();
    buy.globalSequenceNumber = domain::CommandSequence{777};
    buy.orderId = domain::OrderId{888};
    buy.shard_id = 99;
    auto sell = newOrder("SELL");
    sell.type = orderType::SELL;
    sell.price = domain::Price{1'236};
    const std::array commands{buy, sell, cancel()};
    for (const auto& command : commands) {
        ASSERT_EQ(admission.reserve(command).status, core::admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(ingress.push(command));
    }

    for (std::size_t index = 0; index < commands.size(); ++index) {
        script.probe = completionProbe(commands[index], index + 1);
        script.expectedCount = index;
        script.expectedBytes = writer->committedByteCount();
        ASSERT_TRUE(sequencer.step());
        EXPECT_EQ(writer->committedCommandCount(), index + 1);
        EXPECT_EQ(script.syncs, index + 1);
        sequenceMessage delivered{};
        ASSERT_TRUE(matching.pop(delivered));
        EXPECT_EQ(delivered.globalSequenceNumber, domain::CommandSequence{index + 1});
        EXPECT_EQ(delivered.orderId, domain::OrderId{index < 2 ? index + 1 : 0});
        EXPECT_EQ(delivered.clientCommandId, commands[index].clientCommandId);
        EXPECT_EQ(delivered.targetOrderId, commands[index].targetOrderId);
        EXPECT_EQ(delivered.shard_id, commands[index].shard_id);
        EXPECT_EQ(admission.markSequenced(delivered), core::admission::MarkSequencedStatus::IDEMPOTENT);
        const auto result = state.processCommand(delivered);
        EXPECT_EQ(result->correlation().exchangeRunId, RUN_ID);
        EXPECT_EQ(admission.complete(result), core::admission::CompletionStatus::COMPLETED);
        EXPECT_EQ(admission.completedResult(commands[index]), result);
    }
    EXPECT_FALSE(sequencer.step());
    EXPECT_FALSE(sequencer.journalAppendFailure().has_value());
    storage::LoadedRunJournalV1 journal;
    ASSERT_EQ(storage::loadRunJournalV1(path(), journal).outcome, storage::RunJournalLoadOutcome::VALID);
    EXPECT_EQ(journal.header, header());
    ASSERT_EQ(journal.commands.size(), 3U);
    const auto& storedBuy = std::get<storage::JournalNewOrderV1>(journal.commands[0]);
    EXPECT_EQ(storedBuy.side, domain::Side::BUY);
    EXPECT_EQ(storedBuy.commandSequence, domain::CommandSequence{1});
    EXPECT_EQ(storedBuy.clientId, buy.clientId);
    EXPECT_EQ(storedBuy.instrumentId, buy.instrumentId);
    EXPECT_EQ(storedBuy.configurationVersion, buy.configurationVersion);
    EXPECT_EQ(storedBuy.price, buy.price);
    EXPECT_EQ(storedBuy.quantity, buy.quantity);
    EXPECT_EQ(storedBuy.timeInForce, buy.tif);
    EXPECT_EQ(storedBuy.exchangeRunId, RUN_ID);
    EXPECT_EQ(storedBuy.behavioralRulesVersion, header().behavioralRulesVersion);
    const auto& storedSell = std::get<storage::JournalNewOrderV1>(journal.commands[1]);
    EXPECT_EQ(storedSell.side, domain::Side::SELL);
    EXPECT_EQ(storedSell.configurationVersion, sell.configurationVersion);
    EXPECT_EQ(storedSell.price, sell.price);
    const auto& storedCancel = std::get<storage::JournalCancelV1>(journal.commands[2]);
    EXPECT_EQ(storedCancel.commandSequence, domain::CommandSequence{3});
    EXPECT_EQ(storedCancel.targetOrderId, domain::TargetOrderId{1});
    EXPECT_EQ(journal.validCommittedByteCount, writer->committedByteCount());
}

TEST_F(JournalSequencerTest, RecoveredWriterContinuesItsSequenceAndCompletedAdmissionState) {
    const auto original = newOrder();
    {
        std::unique_ptr<storage::RunJournalWriterV1> writer;
        ASSERT_EQ(storage::createRunJournalWriterV1(path(), header(), writer).outcome,
                  storage::RunJournalCreateOutcome::CREATED);
        core::SharedQueue<sequenceMessage> ingress(1);
        core::SharedQueue<sequenceMessage> matching(1);
        core::admission::CommandAdmissionIndex admission(10, RUN_ID);
        core::admission::CommandAdmissionIndexTestAccess::setAdmissionOpen(admission, true);
        TestableSequencer sequencer(ingress, matching, admission, *writer);
        ASSERT_EQ(admission.reserve(original).status, core::admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(ingress.push(original));
        ASSERT_TRUE(sequencer.step());
    }
    core::RunStateV1 recovered;
    ASSERT_EQ(core::recovery::recoverRunV1(path(), recovered, RUN_ID).outcome,
              core::recovery::RecoveredRunOutcome::RECOVERED);
    const auto priorResult = recovered.admissionIndex->completedResult(original);
    ASSERT_NE(priorResult, nullptr);
    EXPECT_EQ(recovered.journalWriter->nextCommandSequence(), domain::CommandSequence{2});
    core::SharedQueue<sequenceMessage> ingress(1);
    core::SharedQueue<sequenceMessage> matching(1);
    TestableSequencer sequencer(ingress, matching, *recovered.admissionIndex, *recovered.journalWriter);
    const auto next = cancel();
    core::admission::CommandAdmissionIndexTestAccess::setAdmissionOpen(*recovered.admissionIndex, true);
    ASSERT_EQ(recovered.admissionIndex->reserve(next).status, core::admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(ingress.push(next));

    ASSERT_TRUE(sequencer.step());

    sequenceMessage delivered{};
    ASSERT_TRUE(matching.pop(delivered));
    EXPECT_EQ(delivered.globalSequenceNumber, domain::CommandSequence{2});
    EXPECT_EQ(delivered.orderId, domain::OrderId{});
    EXPECT_EQ(delivered.targetOrderId, domain::TargetOrderId{1});
    const auto result = recovered.matchingState->processCommand(delivered);
    ASSERT_EQ(result->events().size(), 1U);
    EXPECT_TRUE(std::holds_alternative<domain::OrderCancelled>(result->events()[0]));
    EXPECT_EQ(recovered.admissionIndex->complete(result), core::admission::CompletionStatus::COMPLETED);
    EXPECT_EQ(recovered.admissionIndex->completedResult(original), priorResult);
    EXPECT_EQ(recovered.admissionIndex->completedResult(next), result);
    EXPECT_TRUE(recovered.matchingState->snapshot().activeOrders.empty());
    EXPECT_EQ(recovered.journalWriter->committedCommandCount(), 2U);
    EXPECT_EQ(recovered.journalWriter->nextCommandSequence(), domain::CommandSequence{3});
    storage::LoadedRunJournalV1 journal;
    ASSERT_EQ(storage::loadRunJournalV1(path(), journal).outcome, storage::RunJournalLoadOutcome::VALID);
    ASSERT_EQ(journal.commands.size(), 2U);
    EXPECT_EQ(std::get<storage::JournalCancelV1>(journal.commands[1]).commandSequence, domain::CommandSequence{2});
}

TEST_F(JournalSequencerTest, MatchingSaturationRetriesOnlyHandoffAndNeverReappendsOrOvertakes) {
    AppendScript script;
    std::unique_ptr<storage::RunJournalWriterV1> writer;
    ASSERT_EQ(storage::detail::createRunJournalWriterV1WithHooks(path(), header(),
                                                                 {&script, scriptedWrite, scriptedSync}, writer)
                  .outcome,
              storage::RunJournalCreateOutcome::CREATED);
    core::SharedQueue<sequenceMessage> ingress(2);
    core::SharedQueue<sequenceMessage> matching(1);
    core::admission::CommandAdmissionIndex admission(10, RUN_ID);
    core::admission::CommandAdmissionIndexTestAccess::setAdmissionOpen(admission, true);
    TestableSequencer sequencer(ingress, matching, admission, *writer);
    const auto first = newOrder();
    const auto second = cancel();
    ASSERT_EQ(admission.reserve(first).status, core::admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_EQ(admission.reserve(second).status, core::admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(ingress.push(first));
    ASSERT_TRUE(ingress.push(second));
    ASSERT_TRUE(matching.push(newOrder("BLOCKER")));
    EXPECT_FALSE(sequencer.step());
    ASSERT_TRUE(sequencer.hasPendingSequencedCommand());
    EXPECT_EQ(writer->committedCommandCount(), 1U);
    const auto firstBytes = writer->committedByteCount();
    for (int retry = 0; retry < 3; ++retry) {
        EXPECT_FALSE(sequencer.drain());
        EXPECT_EQ(writer->committedByteCount(), firstBytes);
        EXPECT_EQ(script.writes, 1);
        EXPECT_EQ(script.syncs, 1);
    }
    sequenceMessage delivered{};
    ASSERT_TRUE(matching.pop(delivered));
    EXPECT_EQ(delivered.clientCommandId, domain::ClientCommandId{"BLOCKER"});
    ASSERT_TRUE(sequencer.step());
    EXPECT_EQ(script.syncs, 1);
    EXPECT_FALSE(sequencer.step());
    ASSERT_TRUE(sequencer.pendingCommand().has_value());
    EXPECT_EQ(sequencer.pendingCommand()->clientCommandId, second.clientCommandId);
    EXPECT_EQ(sequencer.pendingCommand()->globalSequenceNumber, domain::CommandSequence{2});
    const auto secondBytes = writer->committedByteCount();
    EXPECT_FALSE(sequencer.step());
    EXPECT_EQ(writer->committedByteCount(), secondBytes);
    ASSERT_TRUE(matching.pop(delivered));
    EXPECT_EQ(delivered.clientCommandId, first.clientCommandId);
    EXPECT_EQ(delivered.globalSequenceNumber, domain::CommandSequence{1});
    ASSERT_TRUE(sequencer.step());
    ASSERT_TRUE(matching.pop(delivered));
    EXPECT_EQ(delivered.clientCommandId, second.clientCommandId);
    EXPECT_EQ(delivered.globalSequenceNumber, domain::CommandSequence{2});
    EXPECT_EQ(delivered.targetOrderId, domain::TargetOrderId{1});
    EXPECT_FALSE(sequencer.step());
    EXPECT_EQ(script.writes, 2);
    EXPECT_EQ(script.syncs, 2);
    EXPECT_EQ(writer->committedCommandCount(), 2U);
    EXPECT_EQ(std::filesystem::file_size(path()), writer->committedByteCount());
}

TEST_F(JournalSequencerTest, WriteAndSyncFailuresRetainReservationAndBlockAllLaterConsumptionWithoutRetry) {
    for (int mode = 0; mode < 4; ++mode) {
        const auto journalPath = path().string() + std::to_string(mode);
        AppendScript script{.failWrite = mode == 3 ? 0 : (mode == 2 ? 2 : 1),
                            .zeroWrite = mode == 1,
                            .failSync = mode == 3,
                            .maximumWrite = mode == 2 ? 8U : std::numeric_limits<std::size_t>::max()};
        std::unique_ptr<storage::RunJournalWriterV1> writer;
        ASSERT_EQ(storage::detail::createRunJournalWriterV1WithHooks(journalPath, header(),
                                                                     {&script, scriptedWrite, scriptedSync}, writer)
                      .outcome,
                  storage::RunJournalCreateOutcome::CREATED);
        const auto initialBytes = writer->committedByteCount();
        core::SharedQueue<sequenceMessage> ingress(2);
        core::SharedQueue<sequenceMessage> matching(1);
        core::admission::CommandAdmissionIndex admission(10, RUN_ID);
        core::admission::CommandAdmissionIndexTestAccess::setAdmissionOpen(admission, true);
        TestableSequencer sequencer(ingress, matching, admission, *writer);
        const auto first = mode == 1 ? cancel() : newOrder();
        const auto later = newOrder("LATER");
        ASSERT_EQ(admission.reserve(first).status, core::admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_EQ(admission.reserve(later).status, core::admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(ingress.push(first));
        ASSERT_TRUE(ingress.push(later));

        EXPECT_FALSE(sequencer.step());

        ASSERT_TRUE(sequencer.pendingCommand().has_value());
        EXPECT_EQ(sequencer.pendingCommand()->clientCommandId, first.clientCommandId);
        EXPECT_EQ(sequencer.pendingCommand()->globalSequenceNumber, domain::CommandSequence{1});
        EXPECT_FALSE(sequencer.hasPendingSequencedCommand());
        ASSERT_TRUE(sequencer.journalAppendFailure().has_value());
        const auto failure = *sequencer.journalAppendFailure();
        EXPECT_EQ(failure.outcome, mode < 2 ? storage::RunJournalAppendOutcome::IO_FAILURE
                                            : storage::RunJournalAppendOutcome::RECOVERY_REQUIRED);
        EXPECT_EQ(failure.systemError, mode == 0 || mode == 2 ? ENOSPC : EIO);
        EXPECT_EQ(failure.error, storage::RunJournalAppendError::NONE);
        EXPECT_EQ(failure.codecError, storage::JournalCommandCodecError::NONE);
        EXPECT_TRUE(matching.empty());
        EXPECT_EQ(admission.complete(completionProbe(first, 1)), core::admission::CompletionStatus::WRONG_STATE);
        EXPECT_EQ(writer->committedCommandCount(), 0U);
        EXPECT_EQ(writer->committedByteCount(), initialBytes);
        EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{1});
        const auto writes = script.writes;
        const auto syncs = script.syncs;
        const auto evidenceSize = std::filesystem::file_size(journalPath);
        if (mode == 3) {
            EXPECT_GT(evidenceSize, initialBytes);
        } else {
            EXPECT_EQ(evidenceSize, initialBytes + (mode == 2 ? 8U : 0U));
        }
        for (int retry = 0; retry < 3; ++retry) {
            EXPECT_FALSE(sequencer.drain());
        }
        EXPECT_EQ(script.writes, writes);
        EXPECT_EQ(script.syncs, syncs);
        EXPECT_EQ(std::filesystem::file_size(journalPath), evidenceSize);
        EXPECT_TRUE(matching.empty());
        EXPECT_EQ(sequencer.journalAppendFailure()->outcome, failure.outcome);
        EXPECT_EQ(sequencer.journalAppendFailure()->systemError, failure.systemError);
        sequenceMessage queued{};
        ASSERT_TRUE(ingress.pop(queued));
        EXPECT_EQ(queued.clientCommandId, later.clientCommandId);
        EXPECT_FALSE(ingress.pop(queued));
    }
}

TEST_F(JournalSequencerTest, CountAndExactByteCapacityRefusalsWriteNothingAndDoNotConsumeASequence) {
    for (const bool byteLimit : {false, true}) {
        auto configuration = header();
        configuration.maxRunCommands = byteLimit ? 10 : 1;
        std::vector<std::byte> encoded;
        ASSERT_EQ(storage::encodeRunHeaderV1(configuration, encoded), storage::RunHeaderCodecError::NONE);
        if (byteLimit) {
            configuration.maxRunJournalBytes = encoded.size();
        }
        AppendScript script;
        std::unique_ptr<storage::RunJournalWriterV1> writer;
        const auto journalPath = path().string() + (byteLimit ? "-bytes" : "-count");
        ASSERT_EQ(storage::detail::createRunJournalWriterV1WithHooks(journalPath, configuration,
                                                                     {&script, scriptedWrite, scriptedSync}, writer)
                      .outcome,
                  storage::RunJournalCreateOutcome::CREATED);
        core::SharedQueue<sequenceMessage> ingress(2);
        core::SharedQueue<sequenceMessage> matching(1);
        core::admission::CommandAdmissionIndex admission(10, RUN_ID);
        core::admission::CommandAdmissionIndexTestAccess::setAdmissionOpen(admission, true);
        TestableSequencer sequencer(ingress, matching, admission, *writer);
        const auto first = newOrder();
        const auto blocked = cancel();
        ASSERT_EQ(admission.reserve(first).status, core::admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_EQ(admission.reserve(blocked).status, core::admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(ingress.push(first));
        ASSERT_TRUE(ingress.push(blocked));
        const auto headerBytes = writer->committedByteCount();
        if (!byteLimit) {
            ASSERT_TRUE(sequencer.step());
            sequenceMessage delivered{};
            ASSERT_TRUE(matching.pop(delivered));
        }
        const auto count = writer->committedCommandCount();
        const auto bytes = writer->committedByteCount();
        const auto next = writer->nextCommandSequence();

        EXPECT_FALSE(sequencer.step());

        ASSERT_TRUE(sequencer.journalAppendFailure().has_value());
        EXPECT_EQ(sequencer.journalAppendFailure()->outcome, storage::RunJournalAppendOutcome::CAPACITY_REACHED);
        EXPECT_EQ(sequencer.journalAppendFailure()->systemError, 0);
        EXPECT_EQ(writer->committedCommandCount(), count);
        EXPECT_EQ(writer->committedByteCount(), bytes);
        EXPECT_EQ(writer->nextCommandSequence(), next);
        EXPECT_EQ(std::filesystem::file_size(journalPath), bytes);
        EXPECT_EQ(script.writes, byteLimit ? 0 : 1);
        EXPECT_EQ(script.syncs, byteLimit ? 0 : 1);
        EXPECT_TRUE(matching.empty());
        EXPECT_FALSE(sequencer.hasPendingSequencedCommand());
        EXPECT_FALSE(sequencer.drain());
        EXPECT_EQ(admission.complete(completionProbe(byteLimit ? first : blocked, next.value())),
                  core::admission::CompletionStatus::WRONG_STATE);
        EXPECT_GE(bytes, headerBytes);
        if (byteLimit) {
            sequenceMessage later{};
            ASSERT_TRUE(ingress.pop(later));
            EXPECT_EQ(later.clientCommandId, blocked.clientCommandId);
        }
    }
}

TEST_F(JournalSequencerTest, ShortWritesAndInterruptedWritesAndSyncsCommitOnlyOneFrame) {
    AppendScript script{.interruptWrite = true, .interruptSync = true, .maximumWrite = 8};
    std::unique_ptr<storage::RunJournalWriterV1> writer;
    ASSERT_EQ(storage::detail::createRunJournalWriterV1WithHooks(path(), header(),
                                                                 {&script, scriptedWrite, scriptedSync}, writer)
                  .outcome,
              storage::RunJournalCreateOutcome::CREATED);
    core::SharedQueue<sequenceMessage> ingress(1);
    core::SharedQueue<sequenceMessage> matching(1);
    core::admission::CommandAdmissionIndex admission(10, RUN_ID);
    core::admission::CommandAdmissionIndexTestAccess::setAdmissionOpen(admission, true);
    TestableSequencer sequencer(ingress, matching, admission, *writer);
    const auto command = cancel();
    ASSERT_EQ(admission.reserve(command).status, core::admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(ingress.push(command));
    ASSERT_TRUE(sequencer.step());
    EXPECT_GT(script.writes, 2);
    EXPECT_EQ(script.syncs, 2);
    EXPECT_EQ(writer->committedCommandCount(), 1U);
    storage::LoadedRunJournalV1 journal;
    ASSERT_EQ(storage::loadRunJournalV1(path(), journal).outcome, storage::RunJournalLoadOutcome::VALID);
    ASSERT_EQ(journal.commands.size(), 1U);
    EXPECT_EQ(std::get<storage::JournalCancelV1>(journal.commands[0]).commandSequence, domain::CommandSequence{1});
}

TEST_F(JournalSequencerTest, ConstructorRejectsUnboundOrWrongRunAdmissionBeforeConsumption) {
    std::unique_ptr<storage::RunJournalWriterV1> writer;
    ASSERT_EQ(storage::createRunJournalWriterV1(path(), header(), writer).outcome,
              storage::RunJournalCreateOutcome::CREATED);
    core::SharedQueue<sequenceMessage> ingress(1);
    core::SharedQueue<sequenceMessage> matching(1);
    ASSERT_TRUE(ingress.push(newOrder()));
    core::admission::CommandAdmissionIndex unbound(10);
    core::admission::CommandAdmissionIndex wrongRun(10, domain::ExchangeRunId{43});
    EXPECT_THROW((TestableSequencer{ingress, matching, unbound, *writer}), std::invalid_argument);
    EXPECT_THROW((TestableSequencer{ingress, matching, wrongRun, *writer}), std::invalid_argument);
    EXPECT_EQ(writer->committedCommandCount(), 0U);
    EXPECT_TRUE(matching.empty());
    sequenceMessage retained{};
    EXPECT_TRUE(ingress.pop(retained));
}

TEST_F(JournalSequencerTest, WideConfigurationVersionCannotTruncateIntoAValidHeaderMember) {
    std::unique_ptr<storage::RunJournalWriterV1> writer;
    ASSERT_EQ(storage::createRunJournalWriterV1(path(), header(), writer).outcome,
              storage::RunJournalCreateOutcome::CREATED);
    core::SharedQueue<sequenceMessage> ingress(1);
    core::SharedQueue<sequenceMessage> matching(1);
    core::admission::CommandAdmissionIndex admission(10, RUN_ID);
    core::admission::CommandAdmissionIndexTestAccess::setAdmissionOpen(admission, true);
    TestableSequencer sequencer(ingress, matching, admission, *writer);
    auto boundary = newOrder("MAX_VERSION");
    boundary.instrumentId = domain::InstrumentId{200};
    boundary.configurationVersion = std::numeric_limits<std::uint32_t>::max();
    boundary.price = domain::Price{std::numeric_limits<std::uint64_t>::max()};
    ASSERT_EQ(admission.reserve(boundary).status, core::admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(ingress.push(boundary));
    ASSERT_TRUE(sequencer.step());
    sequenceMessage delivered{};
    ASSERT_TRUE(matching.pop(delivered));
    EXPECT_EQ(delivered.configurationVersion, boundary.configurationVersion);
    storage::LoadedRunJournalV1 journal;
    ASSERT_EQ(storage::loadRunJournalV1(path(), journal).outcome, storage::RunJournalLoadOutcome::VALID);
    ASSERT_EQ(journal.commands.size(), 1U);
    const auto& persisted = std::get<storage::JournalNewOrderV1>(journal.commands[0]);
    EXPECT_EQ(persisted.configurationVersion, std::numeric_limits<std::uint32_t>::max());
    EXPECT_EQ(persisted.price, boundary.price);
    auto command = newOrder();
    command.configurationVersion += std::uint64_t{1} << 32;
    ASSERT_EQ(admission.reserve(command).status, core::admission::AdmissionStatus::FIRST_SUBMISSION);
    ASSERT_TRUE(ingress.push(command));
    const auto bytes = writer->committedByteCount();
    EXPECT_FALSE(sequencer.step());
    ASSERT_TRUE(sequencer.journalAppendFailure().has_value());
    EXPECT_EQ(sequencer.journalAppendFailure()->outcome, storage::RunJournalAppendOutcome::INVALID_COMMAND);
    EXPECT_EQ(sequencer.journalAppendFailure()->error,
              storage::RunJournalAppendError::INSTRUMENT_CONFIGURATION_MISMATCH);
    EXPECT_EQ(writer->committedByteCount(), bytes);
    EXPECT_EQ(std::filesystem::file_size(path()), bytes);
    EXPECT_TRUE(matching.empty());
}

TEST_F(JournalSequencerTest, RepresentationAndWriterValidationFailuresRetainWorkWithoutMatching) {
    for (int mode = 0; mode < 5; ++mode) {
        std::unique_ptr<storage::RunJournalWriterV1> writer;
        const auto journalPath = path().string() + std::to_string(mode);
        ASSERT_EQ(storage::createRunJournalWriterV1(journalPath, header(), writer).outcome,
                  storage::RunJournalCreateOutcome::CREATED);
        core::SharedQueue<sequenceMessage> ingress(2);
        core::SharedQueue<sequenceMessage> matching(1);
        core::admission::CommandAdmissionIndex admission(10, RUN_ID);
        core::admission::CommandAdmissionIndexTestAccess::setAdmissionOpen(admission, true);
        TestableSequencer sequencer(ingress, matching, admission, *writer);
        auto command = mode == 1 ? cancel() : newOrder();
        auto expectedCodec = storage::JournalCommandCodecError::NONE;
        auto expectedError = storage::RunJournalAppendError::NONE;
        switch (mode) {
            case 0:
                command.clientCommandId.reset();
                expectedCodec = storage::JournalCommandCodecError::INVALID_CLIENT_COMMAND_ID;
                break;
            case 1:
                command.targetOrderId.reset();
                expectedCodec = storage::JournalCommandCodecError::ZERO_TARGET_ORDER_ID;
                break;
            case 2:
                command.type = orderType::CANCELREJ;
                expectedCodec = storage::JournalCommandCodecError::UNSUPPORTED_RECORD_TYPE;
                break;
            case 3:
                command.price = {};
                expectedCodec = storage::JournalCommandCodecError::ZERO_PRICE;
                break;
            case 4:
                command.configurationVersion = 8;
                expectedError = storage::RunJournalAppendError::INSTRUMENT_CONFIGURATION_MISMATCH;
                break;
        }
        // Deliberately inject invalid ingress to test fail-closed representation/codec boundaries.
        ASSERT_TRUE(ingress.push(command));
        ASSERT_TRUE(ingress.push(newOrder("LATER")));
        const auto bytes = writer->committedByteCount();
        EXPECT_FALSE(sequencer.step());
        ASSERT_TRUE(sequencer.journalAppendFailure().has_value());
        EXPECT_EQ(sequencer.journalAppendFailure()->outcome, storage::RunJournalAppendOutcome::INVALID_COMMAND);
        EXPECT_EQ(sequencer.journalAppendFailure()->codecError, expectedCodec);
        EXPECT_EQ(sequencer.journalAppendFailure()->error, expectedError);
        EXPECT_EQ(sequencer.journalAppendFailure()->systemError, 0);
        EXPECT_TRUE(sequencer.pendingCommand().has_value());
        EXPECT_FALSE(sequencer.hasPendingSequencedCommand());
        EXPECT_FALSE(sequencer.drain());
        EXPECT_TRUE(matching.empty());
        EXPECT_EQ(writer->committedCommandCount(), 0U);
        EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{1});
        EXPECT_EQ(std::filesystem::file_size(journalPath), bytes);
        sequenceMessage later{};
        ASSERT_TRUE(ingress.pop(later));
        EXPECT_EQ(later.clientCommandId, domain::ClientCommandId{"LATER"});
    }
}

TEST_F(JournalSequencerTest, ActiveProductionCycleDrainsCommittedCommandsInMultiClientFifoOrder) {
    std::unique_ptr<storage::RunJournalWriterV1> writer;
    ASSERT_EQ(storage::createRunJournalWriterV1(path(), header(), writer).outcome,
              storage::RunJournalCreateOutcome::CREATED);
    core::SharedQueue<sequenceMessage> ingress(3);
    core::SharedQueue<sequenceMessage> matching(3);
    core::admission::CommandAdmissionIndex admission(10, RUN_ID);
    core::admission::CommandAdmissionIndexTestAccess::setAdmissionOpen(admission, true);
    TestableSequencer sequencer(ingress, matching, admission, *writer);
    for (std::uint64_t index = 0; index < 3; ++index) {
        auto command = newOrder();
        command.clientId = domain::ClientId{index + 11};
        ASSERT_EQ(admission.reserve(command).status, core::admission::AdmissionStatus::FIRST_SUBMISSION);
        ASSERT_TRUE(ingress.push(command));
    }

    EXPECT_TRUE(sequencer.drain());

    EXPECT_FALSE(sequencer.drain());
    EXPECT_EQ(writer->committedCommandCount(), 3U);
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{4});
    for (std::uint64_t index = 0; index < 3; ++index) {
        sequenceMessage delivered{};
        ASSERT_TRUE(matching.pop(delivered));
        EXPECT_EQ(delivered.clientId, domain::ClientId{index + 11});
        EXPECT_EQ(delivered.globalSequenceNumber, domain::CommandSequence{index + 1});
        EXPECT_EQ(delivered.orderId, domain::OrderId{index + 1});
    }
    EXPECT_TRUE(matching.empty());
}

TEST_F(JournalSequencerTest, PostCommitAdmissionInvariantFailureCannotRetryAppendOrHandoff) {
    std::unique_ptr<storage::RunJournalWriterV1> writer;
    ASSERT_EQ(storage::createRunJournalWriterV1(path(), header(), writer).outcome,
              storage::RunJournalCreateOutcome::CREATED);
    core::SharedQueue<sequenceMessage> ingress(2);
    core::SharedQueue<sequenceMessage> matching(1);
    core::admission::CommandAdmissionIndex admission(10, RUN_ID);
    core::admission::CommandAdmissionIndexTestAccess::setAdmissionOpen(admission, true);
    TestableSequencer sequencer(ingress, matching, admission, *writer);
    ASSERT_TRUE(ingress.push(newOrder()));
    ASSERT_TRUE(ingress.push(cancel()));
    EXPECT_THROW(sequencer.step(), std::logic_error);
    EXPECT_EQ(writer->committedCommandCount(), 1U);
    EXPECT_TRUE(sequencer.pendingCommand().has_value());
    EXPECT_FALSE(sequencer.hasPendingSequencedCommand());
    EXPECT_TRUE(matching.empty());
    EXPECT_FALSE(sequencer.drain());
    EXPECT_EQ(writer->committedCommandCount(), 1U);
    sequenceMessage later{};
    ASSERT_TRUE(ingress.pop(later));
    EXPECT_EQ(later.type, orderType::CANCEL);
}

} // namespace
} // namespace exchange::sequencer
