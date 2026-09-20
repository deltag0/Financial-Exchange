#include "recovered_run.hpp"

#include "recovered_run_internal.hpp"

#include "bus.hpp"
#include "matching_engine.hpp"
#include "shared_queue.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <type_traits>
#include <unistd.h>
#include <utility>
#include <variant>
#include <vector>

namespace exchange::core::recovery {
namespace {

constexpr domain::ExchangeRunId RUN_ID{42};

static_assert(!std::is_copy_constructible_v<RunStateV1>);

class RecoveredRunTestDirectory final {
public:
    RecoveredRunTestDirectory() {
        std::array<char, 53> pathTemplate{};
        constexpr char TEMPLATE[] = "/tmp/exchange-recovered-run-test-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), pathTemplate.begin());
        char *created = ::mkdtemp(pathTemplate.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~RecoveredRunTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    RecoveredRunTestDirectory(const RecoveredRunTestDirectory &) = delete;
    RecoveredRunTestDirectory &operator=(const RecoveredRunTestDirectory &) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return !path_.empty();
    }

    [[nodiscard]] std::filesystem::path journalPath(const std::string &name) const {
        return path_ / name;
    }

private:
    std::filesystem::path path_{};
};

storage::RunHeaderV1 recoveredHeader(const domain::ExchangeRunId runId = RUN_ID) {
    return {
        .exchangeRunId = runId,
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = 10,
        .maxRunJournalBytes = 64 * 1024,
        .instruments = {{domain::InstrumentId{1}, 1}},
    };
}

storage::JournalNewOrderV1 recoveredOrder(const std::uint64_t sequence, const domain::ClientId clientId,
                                          const std::string &clientCommandId, const domain::Side side,
                                          const std::uint64_t price, const std::uint64_t quantity,
                                          const task::TimeInForce timeInForce = task::TimeInForce::GTC,
                                          const domain::ExchangeRunId runId = RUN_ID) {
    return {
        .exchangeRunId = runId,
        .commandSequence = domain::CommandSequence{sequence},
        .behavioralRulesVersion = 1,
        .configurationVersion = 1,
        .clientId = clientId,
        .instrumentId = domain::InstrumentId{1},
        .clientCommandId = domain::ClientCommandId{clientCommandId},
        .side = side,
        .timeInForce = timeInForce,
        .price = domain::Price{price},
        .quantity = domain::Quantity{quantity},
    };
}

storage::JournalCancelV1 recoveredCancel(const std::uint64_t sequence, const domain::ClientId clientId,
                                         const std::string &clientCommandId, const std::uint64_t targetOrderId,
                                         const domain::ExchangeRunId runId = RUN_ID) {
    return {
        .exchangeRunId = runId,
        .commandSequence = domain::CommandSequence{sequence},
        .behavioralRulesVersion = 1,
        .configurationVersion = 1,
        .clientId = clientId,
        .instrumentId = domain::InstrumentId{1},
        .clientCommandId = domain::ClientCommandId{clientCommandId},
        .targetOrderId = domain::TargetOrderId{targetOrderId},
    };
}

void createJournal(const std::filesystem::path &path, const storage::RunHeaderV1 &header,
                   const std::vector<storage::JournalCommandV1> &commands, std::uint64_t &committedByteCount) {
    std::unique_ptr<storage::RunJournalWriterV1> writer;
    ASSERT_EQ(storage::createRunJournalWriterV1(path, header, writer).outcome,
              storage::RunJournalCreateOutcome::CREATED);
    ASSERT_NE(writer, nullptr);
    for (const storage::JournalCommandV1 &command : commands) {
        const storage::RunJournalAppendResult append =
            std::visit([&](const auto &typed) { return writer->append(typed); }, command);
        ASSERT_EQ(append.outcome, storage::RunJournalAppendOutcome::COMMITTED);
    }
    committedByteCount = writer->committedByteCount();
    writer.reset();
}

void appendTail(const std::filesystem::path &path, const std::span<const std::byte> tail) {
    std::ofstream stream(path, std::ios::binary | std::ios::app);
    ASSERT_TRUE(stream.is_open());
    stream.write(reinterpret_cast<const char *>(tail.data()), static_cast<std::streamsize>(tail.size()));
    ASSERT_TRUE(stream.good());
}

void writeCorruptFile(const std::filesystem::path &path) {
    const std::array<char, 4> bytes{'F', 'X', 'J', 'R'};
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());
    stream.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(stream.good());
}

struct RecoveredPointers final {
    storage::RunJournalWriterV1 *writer;
    matching_engine::MatchingState *matchingState;
    admission::CommandAdmissionIndex *admissionIndex;
};

RecoveredPointers installSentinel(const std::filesystem::path &path, RunStateV1 &output) {
    std::uint64_t ignoredByteCount = 0;
    createJournal(path, recoveredHeader(domain::ExchangeRunId{999}), {}, ignoredByteCount);
    EXPECT_EQ(recoverRunV1(path, output).outcome, RecoveredRunOutcome::RECOVERED);
    return {output.journalWriter.get(), output.matchingState.get(), output.admissionIndex.get()};
}

void expectUnchanged(const RunStateV1 &output, const RecoveredPointers original) {
    EXPECT_EQ(output.journalWriter.get(), original.writer);
    EXPECT_EQ(output.matchingState.get(), original.matchingState);
    EXPECT_EQ(output.admissionIndex.get(), original.admissionIndex);
    ASSERT_NE(output.matchingState, nullptr);
    EXPECT_EQ(output.matchingState->snapshot().exchangeRunId, domain::ExchangeRunId{999});
}

matching_engine::RunJournalReplayResult failReplay(const storage::PreparedRunJournalV1 &,
                                                   matching_engine::ReplayedRunJournalV1 &) noexcept {
    return {matching_engine::RunJournalReplayOutcome::INVALID_JOURNAL,
            matching_engine::RunJournalReplayError::COMMAND_SEQUENCE_MISMATCH};
}

matching_engine::RunJournalReplayResult failReplayInvariant(const storage::PreparedRunJournalV1 &,
                                                            matching_engine::ReplayedRunJournalV1 &) noexcept {
    return {matching_engine::RunJournalReplayOutcome::INVARIANT_FAILURE,
            matching_engine::RunJournalReplayError::MATCHING_INVARIANT_FAILURE};
}

admission::CommandAdmissionRecoveryResult failAdmission(
    const storage::LoadedRunJournalV1 &, const std::vector<matching_engine::ImmutableCommandResultBatch> &,
    std::unique_ptr<admission::CommandAdmissionIndex> &) noexcept {
    return {admission::CommandAdmissionRecoveryOutcome::INVALID_INPUT,
            admission::CommandAdmissionRecoveryError::RESULT_CORRELATION_MISMATCH};
}

admission::CommandAdmissionRecoveryResult failAdmissionInvariant(
    const storage::LoadedRunJournalV1 &, const std::vector<matching_engine::ImmutableCommandResultBatch> &,
    std::unique_ptr<admission::CommandAdmissionIndex> &) noexcept {
    return {admission::CommandAdmissionRecoveryOutcome::INVARIANT_FAILURE,
            admission::CommandAdmissionRecoveryError::NONE};
}

class InstalledMatchingEngine final : public matching_engine::MatchingEngine {
public:
    InstalledMatchingEngine(core::SharedQueue<sequencer::sequenceMessage> &commands, core::Bus &multicastBus,
                            matching_engine::BoundedCommandResultQueue &results,
                            matching_engine::MatchingState &matchingState)
        : MatchingEngine(&commands, multicastBus, results, matchingState) {}

    bool drain() {
        return drainQueue(sequencerQueue, "recovered-state integration");
    }
};

TEST(RecoveredRunTest, EmptyJournalTransfersOneWriterMatchingStateAndAdmissionIndexForTheSameRun) {
    RecoveredRunTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const std::filesystem::path path = directory.journalPath("empty.fxjr");
    std::uint64_t headerByteCount = 0;
    createJournal(path, recoveredHeader(), {}, headerByteCount);
    RunStateV1 recovered;

    const RecoveredRunResult result = recoverRunV1(path, recovered);

    EXPECT_EQ(result.outcome, RecoveredRunOutcome::RECOVERED);
    EXPECT_EQ(result.preparation.outcome, storage::RunJournalRecoveryOutcome::PREPARED);
    EXPECT_EQ(result.replay.outcome, matching_engine::RunJournalReplayOutcome::REPLAYED);
    EXPECT_EQ(result.admission.outcome, admission::CommandAdmissionRecoveryOutcome::RECONSTRUCTED);
    ASSERT_NE(recovered.journalWriter, nullptr);
    ASSERT_NE(recovered.matchingState, nullptr);
    ASSERT_NE(recovered.admissionIndex, nullptr);
    EXPECT_EQ(recovered.journalWriter->committedCommandCount(), 0U);
    EXPECT_EQ(recovered.journalWriter->committedByteCount(), headerByteCount);
    EXPECT_EQ(recovered.journalWriter->nextCommandSequence(), domain::CommandSequence{1});
    EXPECT_EQ(recovered.matchingState->snapshot().exchangeRunId, RUN_ID);
    EXPECT_EQ(recovered.admissionIndex->capacity(), recoveredHeader().maxRunCommands);
    EXPECT_TRUE(recovered.admissionIndex->statistics() == admission::AdmissionStatistics{});
}

TEST(RecoveredRunTest, MixedJournalRestoresWriterPositionMatchingStateAndExactCompletedLookups) {
    RecoveredRunTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const std::filesystem::path path = directory.journalPath("mixed.fxjr");
    const std::vector<storage::JournalCommandV1> commands{
        recoveredOrder(1, domain::ClientId{101}, "SELL-1", domain::Side::SELL, 100, 10),
        recoveredOrder(2, domain::ClientId{202}, "BUY-2", domain::Side::BUY, 100, 4, task::TimeInForce::IOC),
        recoveredCancel(3, domain::ClientId{101}, "CANCEL-3", 999),
    };
    std::uint64_t committedByteCount = 0;
    createJournal(path, recoveredHeader(), commands, committedByteCount);
    RunStateV1 recovered;

    ASSERT_EQ(recoverRunV1(path, recovered).outcome, RecoveredRunOutcome::RECOVERED);

    ASSERT_NE(recovered.journalWriter, nullptr);
    EXPECT_EQ(recovered.journalWriter->committedCommandCount(), 3U);
    EXPECT_EQ(recovered.journalWriter->committedByteCount(), committedByteCount);
    EXPECT_EQ(recovered.journalWriter->nextCommandSequence(), domain::CommandSequence{4});
    ASSERT_NE(recovered.matchingState, nullptr);
    const matching_engine::MatchingStateSnapshot state = recovered.matchingState->snapshot();
    EXPECT_EQ(state.exchangeRunId, RUN_ID);
    ASSERT_EQ(state.priceLevels.size(), 1U);
    EXPECT_EQ(state.priceLevels[0].side, domain::Side::SELL);
    EXPECT_EQ(state.priceLevels[0].totalQuantity, domain::Quantity{6});
    ASSERT_EQ(state.activeOrders.size(), 1U);
    EXPECT_EQ(state.activeOrders[0].orderId, domain::OrderId{1});
    EXPECT_EQ(state.activeOrders[0].remainingQuantity, domain::Quantity{6});

    sequencer::sequenceMessage sell{};
    sell.orderId = domain::OrderId{1};
    sell.globalSequenceNumber = domain::CommandSequence{1};
    sell.clientId = domain::ClientId{101};
    sell.clientCommandId = domain::ClientCommandId{"SELL-1"};
    sell.instrumentId = domain::InstrumentId{1};
    sell.configurationVersion = 1;
    sell.price = domain::Price{100};
    sell.quantity = domain::Quantity{10};
    sell.type = sequencer::orderType::SELL;
    sell.tif = task::TimeInForce::GTC;

    sequencer::sequenceMessage buy{};
    buy.orderId = domain::OrderId{2};
    buy.globalSequenceNumber = domain::CommandSequence{2};
    buy.clientId = domain::ClientId{202};
    buy.clientCommandId = domain::ClientCommandId{"BUY-2"};
    buy.instrumentId = domain::InstrumentId{1};
    buy.configurationVersion = 1;
    buy.price = domain::Price{100};
    buy.quantity = domain::Quantity{4};
    buy.type = sequencer::orderType::BUY;
    buy.tif = task::TimeInForce::IOC;

    sequencer::sequenceMessage cancel{};
    cancel.targetOrderId = domain::TargetOrderId{999};
    cancel.globalSequenceNumber = domain::CommandSequence{3};
    cancel.clientId = domain::ClientId{101};
    cancel.clientCommandId = domain::ClientCommandId{"CANCEL-3"};
    cancel.instrumentId = domain::InstrumentId{1};
    cancel.configurationVersion = 1;
    cancel.type = sequencer::orderType::CANCEL;
    const auto sellResult = recovered.admissionIndex->completedResult(sell);
    const auto buyResult = recovered.admissionIndex->completedResult(buy);
    const auto cancelResult = recovered.admissionIndex->completedResult(cancel);
    ASSERT_NE(sellResult, nullptr);
    ASSERT_NE(buyResult, nullptr);
    ASSERT_NE(cancelResult, nullptr);
    EXPECT_EQ(sellResult->correlation().exchangeRunId, RUN_ID);
    EXPECT_EQ(sellResult->correlation().commandSequence, domain::CommandSequence{1});
    EXPECT_EQ(buyResult->correlation().commandSequence, domain::CommandSequence{2});
    EXPECT_EQ(cancelResult->correlation().commandSequence, domain::CommandSequence{3});

    storage::JournalNewOrderV1 fourth = recoveredOrder(4, domain::ClientId{303}, "BUY-4", domain::Side::BUY, 90, 1);
    fourth.exchangeRunId = domain::ExchangeRunId{43};
    const storage::RunJournalAppendResult wrongRunAppend = recovered.journalWriter->append(fourth);
    EXPECT_EQ(wrongRunAppend.outcome, storage::RunJournalAppendOutcome::INVALID_COMMAND);
    EXPECT_EQ(wrongRunAppend.error, storage::RunJournalAppendError::EXCHANGE_RUN_ID_MISMATCH);
    EXPECT_EQ(recovered.journalWriter->committedCommandCount(), 3U);
    fourth.exchangeRunId = RUN_ID;
    EXPECT_EQ(recovered.journalWriter->append(fourth).outcome, storage::RunJournalAppendOutcome::COMMITTED);
    EXPECT_EQ(recovered.journalWriter->committedCommandCount(), 4U);
    EXPECT_GT(recovered.journalWriter->committedByteCount(), committedByteCount);
}

TEST(RecoveredRunTest, WorkerBorrowsRecoveredStateMatchesNextCommandAndLeavesStateUsable) {
    RecoveredRunTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const std::filesystem::path path = directory.journalPath("installed-mixed.fxjr");
    const std::vector<storage::JournalCommandV1> commands{
        recoveredOrder(1, domain::ClientId{101}, "SELL-1", domain::Side::SELL, 100, 10),
        recoveredOrder(2, domain::ClientId{202}, "BUY-2", domain::Side::BUY, 100, 4, task::TimeInForce::IOC),
        recoveredCancel(3, domain::ClientId{101}, "CANCEL-3", 999),
    };
    std::uint64_t ignoredCommittedByteCount = 0;
    createJournal(path, recoveredHeader(), commands, ignoredCommittedByteCount);

    core::Bus multicastBus(4);
    std::atomic<core::Bus::cursor_type> multicastCursor{0};
    multicastBus.registerCursor(multicastCursor);
    core::SharedQueue<sequencer::sequenceMessage> commandQueue(4);
    matching_engine::BoundedCommandResultQueue resultQueue(4);
    RunStateV1 recovered;

    ASSERT_EQ(recoverRunV1(path, recovered).outcome, RecoveredRunOutcome::RECOVERED);
    EXPECT_TRUE(resultQueue.empty());
    sequencer::sequenceMessage published{};
    EXPECT_FALSE(multicastBus.read(multicastCursor, published));
    ASSERT_NE(recovered.matchingState, nullptr);
    matching_engine::MatchingState *const recoveredState = recovered.matchingState.get();

    sequencer::sequenceMessage liveBuy{};
    liveBuy.orderId = domain::OrderId{4};
    liveBuy.globalSequenceNumber = domain::CommandSequence{4};
    liveBuy.clientId = domain::ClientId{303};
    liveBuy.clientCommandId = domain::ClientCommandId{"LIVE-BUY-4"};
    liveBuy.instrumentId = domain::InstrumentId{1};
    liveBuy.configurationVersion = 1;
    liveBuy.price = domain::Price{100};
    liveBuy.quantity = domain::Quantity{6};
    liveBuy.type = sequencer::orderType::BUY;
    liveBuy.tif = task::TimeInForce::IOC;
    ASSERT_TRUE(commandQueue.push(liveBuy));

    {
        InstalledMatchingEngine engine(commandQueue, multicastBus, resultQueue, *recovered.matchingState);
        EXPECT_EQ(recovered.matchingState.get(), recoveredState);
        EXPECT_TRUE(engine.drain());

        ASSERT_EQ(resultQueue.size(), 1U);
        matching_engine::ImmutableCommandResultBatch result;
        ASSERT_TRUE(resultQueue.tryPop(result));
        ASSERT_NE(result, nullptr);
        EXPECT_EQ(result->correlation().exchangeRunId, RUN_ID);
        EXPECT_EQ(result->commandSequence(), domain::CommandSequence{4});
        EXPECT_EQ(result->correlation().clientId, liveBuy.clientId);
        EXPECT_EQ(result->correlation().clientCommandId, *liveBuy.clientCommandId);
        ASSERT_EQ(result->events().size(), 1U);
        const auto *trade = std::get_if<domain::Trade>(&result->events()[0]);
        ASSERT_NE(trade, nullptr);
        EXPECT_EQ(trade->eventId, (domain::EventId{domain::CommandSequence{4}, domain::EventIndex{0}, RUN_ID}));
        EXPECT_EQ(trade->makerOrderId, domain::OrderId{1});
        EXPECT_EQ(trade->makerClientId, domain::ClientId{101});
        EXPECT_EQ(trade->takerOrderId, domain::OrderId{4});
        EXPECT_EQ(trade->executionQuantity, domain::Quantity{6});
        EXPECT_TRUE(resultQueue.empty());

        EXPECT_TRUE(multicastBus.read(multicastCursor, published));
        EXPECT_EQ(published.globalSequenceNumber, domain::CommandSequence{4});
        EXPECT_EQ(published.orderId, domain::OrderId{4});
        EXPECT_FALSE(multicastBus.read(multicastCursor, published));
        EXPECT_EQ(engine.multicastWriteFailures(), 0U);
    }

    ASSERT_EQ(recovered.matchingState.get(), recoveredState);
    EXPECT_TRUE(recovered.matchingState->invariantsHold());
    const matching_engine::MatchingStateSnapshot finalState = recoveredState->snapshot();
    EXPECT_EQ(finalState.exchangeRunId, RUN_ID);
    EXPECT_TRUE(finalState.priceLevels.empty());
    EXPECT_TRUE(finalState.activeOrders.empty());

    liveBuy.orderId = domain::OrderId{5};
    liveBuy.globalSequenceNumber = domain::CommandSequence{5};
    liveBuy.clientCommandId = domain::ClientCommandId{"AFTER-WORKER-5"};
    liveBuy.tif = task::TimeInForce::GTC;
    const auto afterWorker = recovered.matchingState->processCommand(liveBuy);
    EXPECT_EQ(afterWorker->correlation().exchangeRunId, RUN_ID);
    EXPECT_EQ(afterWorker->commandSequence(), domain::CommandSequence{5});
    ASSERT_EQ(recovered.matchingState->snapshot().activeOrders.size(), 1U);
    EXPECT_EQ(recovered.matchingState->snapshot().activeOrders[0].orderId, domain::OrderId{5});
}

TEST(RecoveredRunTest, RepairsIncompleteFinalTailBeforeTransferringRecoveredOwners) {
    RecoveredRunTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const std::filesystem::path path = directory.journalPath("tail.fxjr");
    const std::vector<storage::JournalCommandV1> commands{
        recoveredOrder(1, domain::ClientId{101}, "ORDER-1", domain::Side::BUY, 100, 2),
    };
    std::uint64_t committedByteCount = 0;
    createJournal(path, recoveredHeader(), commands, committedByteCount);
    const std::array<std::byte, 7> tail{
        std::byte{0x46}, std::byte{0x58}, std::byte{0x4a}, std::byte{0x52},
        std::byte{0x01}, std::byte{0x02}, std::byte{0x03},
    };
    appendTail(path, tail);
    RunStateV1 recovered;

    const RecoveredRunResult result = recoverRunV1(path, recovered);

    EXPECT_EQ(result.outcome, RecoveredRunOutcome::RECOVERED);
    EXPECT_EQ(result.preparation.validation.outcome, storage::RunJournalLoadOutcome::INCOMPLETE_TAIL);
    EXPECT_EQ(result.preparation.validation.tailOffset, committedByteCount);
    EXPECT_EQ(result.preparation.validation.tailLength, tail.size());
    EXPECT_EQ(result.preparation.preservedTailBytes, std::vector<std::byte>(tail.begin(), tail.end()));
    EXPECT_EQ(std::filesystem::file_size(path), committedByteCount);
    ASSERT_NE(recovered.journalWriter, nullptr);
    EXPECT_EQ(recovered.journalWriter->committedCommandCount(), 1U);
    EXPECT_EQ(recovered.journalWriter->committedByteCount(), committedByteCount);
    EXPECT_EQ(recovered.journalWriter->nextCommandSequence(), domain::CommandSequence{2});
    sequencer::sequenceMessage lookup{};
    lookup.orderId = domain::OrderId{1};
    lookup.globalSequenceNumber = domain::CommandSequence{1};
    lookup.clientId = domain::ClientId{101};
    lookup.clientCommandId = domain::ClientCommandId{"ORDER-1"};
    lookup.instrumentId = domain::InstrumentId{1};
    lookup.configurationVersion = 1;
    lookup.price = domain::Price{100};
    lookup.quantity = domain::Quantity{2};
    lookup.type = sequencer::orderType::BUY;
    lookup.tif = task::TimeInForce::GTC;
    ASSERT_NE(recovered.admissionIndex->completedResult(lookup), nullptr);
}

TEST(RecoveredRunTest, PreparationAndCorruptionFailuresPreserveCallerOutputAndExactCategories) {
    RecoveredRunTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    RunStateV1 output;
    const RecoveredPointers original = installSentinel(directory.journalPath("sentinel.fxjr"), output);

    const RecoveredRunResult missing = recoverRunV1(directory.journalPath("missing.fxjr"), output);
    EXPECT_EQ(missing.outcome, RecoveredRunOutcome::PREPARATION_FAILED);
    EXPECT_EQ(missing.preparation.outcome, storage::RunJournalRecoveryOutcome::MISSING);
    expectUnchanged(output, original);

    const std::filesystem::path corruptPath = directory.journalPath("corrupt.fxjr");
    writeCorruptFile(corruptPath);
    const RecoveredRunResult corrupt = recoverRunV1(corruptPath, output);
    EXPECT_EQ(corrupt.outcome, RecoveredRunOutcome::PREPARATION_FAILED);
    EXPECT_EQ(corrupt.preparation.outcome, storage::RunJournalRecoveryOutcome::CORRUPTION);
    EXPECT_EQ(corrupt.preparation.validation.outcome, storage::RunJournalLoadOutcome::CORRUPTION);
    expectUnchanged(output, original);
}

TEST(RecoveredRunTest, ReplayAndReplayInvariantFailuresPreserveCallerOutputAndStageDetails) {
    RecoveredRunTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const std::filesystem::path path = directory.journalPath("source.fxjr");
    std::uint64_t ignoredByteCount = 0;
    createJournal(path, recoveredHeader(), {}, ignoredByteCount);
    RunStateV1 output;
    const RecoveredPointers original = installSentinel(directory.journalPath("sentinel.fxjr"), output);

    const detail::RecoveredRunHooks replayFailureHooks{
        storage::prepareRunJournalV1,
        failReplay,
        admission::reconstructCommandAdmissionIndex,
    };
    const RecoveredRunResult replayFailure = detail::recoverRunV1WithHooks(path, replayFailureHooks, output);
    EXPECT_EQ(replayFailure.outcome, RecoveredRunOutcome::REPLAY_FAILED);
    EXPECT_EQ(replayFailure.preparation.outcome, storage::RunJournalRecoveryOutcome::PREPARED);
    EXPECT_EQ(replayFailure.replay.outcome, matching_engine::RunJournalReplayOutcome::INVALID_JOURNAL);
    EXPECT_EQ(replayFailure.replay.error, matching_engine::RunJournalReplayError::COMMAND_SEQUENCE_MISMATCH);
    expectUnchanged(output, original);

    const detail::RecoveredRunHooks invariantHooks{
        storage::prepareRunJournalV1,
        failReplayInvariant,
        admission::reconstructCommandAdmissionIndex,
    };
    const RecoveredRunResult invariantFailure = detail::recoverRunV1WithHooks(path, invariantHooks, output);
    EXPECT_EQ(invariantFailure.outcome, RecoveredRunOutcome::REPLAY_FAILED);
    EXPECT_EQ(invariantFailure.replay.outcome, matching_engine::RunJournalReplayOutcome::INVARIANT_FAILURE);
    EXPECT_EQ(invariantFailure.replay.error, matching_engine::RunJournalReplayError::MATCHING_INVARIANT_FAILURE);
    expectUnchanged(output, original);
}

TEST(RecoveredRunTest, AdmissionAndAdmissionInvariantFailuresPreserveCallerOutputAndStageDetails) {
    RecoveredRunTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const std::filesystem::path path = directory.journalPath("source.fxjr");
    std::uint64_t ignoredByteCount = 0;
    createJournal(path, recoveredHeader(), {}, ignoredByteCount);
    RunStateV1 output;
    const RecoveredPointers original = installSentinel(directory.journalPath("sentinel.fxjr"), output);

    const detail::RecoveredRunHooks admissionFailureHooks{
        storage::prepareRunJournalV1,
        matching_engine::replayPreparedRunJournalV1,
        failAdmission,
    };
    const RecoveredRunResult admissionFailure = detail::recoverRunV1WithHooks(path, admissionFailureHooks, output);
    EXPECT_EQ(admissionFailure.outcome, RecoveredRunOutcome::ADMISSION_FAILED);
    EXPECT_EQ(admissionFailure.replay.outcome, matching_engine::RunJournalReplayOutcome::REPLAYED);
    EXPECT_EQ(admissionFailure.admission.outcome, admission::CommandAdmissionRecoveryOutcome::INVALID_INPUT);
    EXPECT_EQ(admissionFailure.admission.error, admission::CommandAdmissionRecoveryError::RESULT_CORRELATION_MISMATCH);
    expectUnchanged(output, original);

    const detail::RecoveredRunHooks invariantHooks{
        storage::prepareRunJournalV1,
        matching_engine::replayPreparedRunJournalV1,
        failAdmissionInvariant,
    };
    const RecoveredRunResult invariantFailure = detail::recoverRunV1WithHooks(path, invariantHooks, output);
    EXPECT_EQ(invariantFailure.outcome, RecoveredRunOutcome::ADMISSION_FAILED);
    EXPECT_EQ(invariantFailure.admission.outcome, admission::CommandAdmissionRecoveryOutcome::INVARIANT_FAILURE);
    expectUnchanged(output, original);
}

} // namespace
} // namespace exchange::core::recovery
