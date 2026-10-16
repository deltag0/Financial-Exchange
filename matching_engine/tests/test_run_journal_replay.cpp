#include "run_journal_replay.hpp"

#include "../../core/bus/include/bus.hpp"
#include "../../core/shared_queue/include/shared_queue.hpp"
#include "../../core/storage/include/run_journal_recovery.hpp"
#include "../../core/storage/include/run_journal_writer.hpp"
#include "../include/matching_engine.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <unistd.h>
#include <variant>
#include <vector>

namespace exchange::matching_engine {
namespace {

constexpr domain::ExchangeRunId RUN_ID{42};

storage::RunHeaderV1 replayHeader(const bool includeSecondInstrument = false) {
    storage::RunHeaderV1 header{
        .exchangeRunId = RUN_ID,
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = 100,
        .maxRunJournalBytes = 1024 * 1024,
        .instruments = {{domain::InstrumentId{1}, 1}},
    };
    if (includeSecondInstrument) {
        header.instruments.push_back({domain::InstrumentId{2}, 1});
    }
    return header;
}

storage::JournalNewOrderV1 replayOrder(const std::uint64_t sequence, const domain::Side side, const std::uint64_t price,
                                       const std::uint64_t quantity,
                                       const core::task::TimeInForce timeInForce = core::task::TimeInForce::GTC,
                                       const domain::InstrumentId instrumentId = domain::InstrumentId{1}) {
    return {
        .exchangeRunId = RUN_ID,
        .commandSequence = domain::CommandSequence{sequence},
        .behavioralRulesVersion = 1,
        .configurationVersion = 1,
        .clientId = domain::ClientId{100 + sequence},
        .instrumentId = instrumentId,
        .clientCommandId = domain::ClientCommandId{"ORDER-" + std::to_string(sequence)},
        .side = side,
        .timeInForce = timeInForce,
        .price = domain::Price{price},
        .quantity = domain::Quantity{quantity},
    };
}

storage::JournalCancelV1 replayCancel(const std::uint64_t sequence, const std::uint64_t targetOrderId,
                                      const domain::ClientId clientId,
                                      const domain::InstrumentId instrumentId = domain::InstrumentId{1}) {
    return {
        .exchangeRunId = RUN_ID,
        .commandSequence = domain::CommandSequence{sequence},
        .behavioralRulesVersion = 1,
        .configurationVersion = 1,
        .clientId = clientId,
        .instrumentId = instrumentId,
        .clientCommandId = domain::ClientCommandId{"CANCEL-" + std::to_string(sequence)},
        .targetOrderId = domain::TargetOrderId{targetOrderId},
    };
}

storage::PreparedRunJournalV1 preparedJournal(std::vector<storage::JournalCommandV1> commands,
                                              const bool includeSecondInstrument = false) {
    storage::PreparedRunJournalV1 prepared;
    prepared.journal.header = replayHeader(includeSecondInstrument);
    prepared.journal.commands = std::move(commands);
    prepared.journal.validCommittedByteCount = 1;
    return prepared;
}

sequencer::sequenceMessage liveMessageFrom(const storage::JournalNewOrderV1 &command) {
    sequencer::sequenceMessage message{};
    message.orderId = domain::orderIdFrom(command.commandSequence);
    message.globalSequenceNumber = command.commandSequence;
    message.clientId = command.clientId;
    message.clientCommandId = command.clientCommandId;
    message.instrumentId = command.instrumentId;
    message.configurationVersion = command.configurationVersion;
    message.price = command.price;
    message.quantity = command.quantity;
    message.type = command.side == domain::Side::BUY ? sequencer::orderType::BUY : sequencer::orderType::SELL;
    message.tif = command.timeInForce;
    return message;
}

sequencer::sequenceMessage liveMessageFrom(const storage::JournalCancelV1 &command) {
    sequencer::sequenceMessage message{};
    message.targetOrderId = command.targetOrderId;
    message.globalSequenceNumber = command.commandSequence;
    message.clientId = command.clientId;
    message.clientCommandId = command.clientCommandId;
    message.instrumentId = command.instrumentId;
    message.configurationVersion = command.configurationVersion;
    message.type = sequencer::orderType::CANCEL;
    return message;
}

void expectEqualResults(const std::vector<ImmutableCommandResultBatch> &left,
                        const std::vector<ImmutableCommandResultBatch> &right) {
    ASSERT_EQ(left.size(), right.size());
    for (std::size_t index = 0; index < left.size(); ++index) {
        ASSERT_NE(left[index], nullptr);
        ASSERT_NE(right[index], nullptr);
        EXPECT_EQ(left[index]->correlation(), right[index]->correlation());
        EXPECT_EQ(left[index]->result(), right[index]->result());
        EXPECT_EQ(left[index]->events(), right[index]->events());
    }
}

class LiveMatchingHarness final : public MatchingEngine {
public:
    LiveMatchingHarness(core::SharedQueue<sequencer::sequenceMessage> &commands, core::Bus &bus,
                        BoundedCommandResultQueue &results, MatchingState &matchingState)
        : MatchingEngine(&commands, bus, results, matchingState) {}

    bool drain() {
        return drainQueue(sequencerQueue, "test");
    }
};

class ReplayTestDirectory final {
public:
    ReplayTestDirectory() {
        std::array<char, 49> pathTemplate{};
        constexpr char TEMPLATE[] = "/tmp/exchange-journal-replay-test-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), pathTemplate.begin());
        char *created = ::mkdtemp(pathTemplate.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~ReplayTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    [[nodiscard]] bool valid() const noexcept {
        return !path_.empty();
    }

    [[nodiscard]] std::filesystem::path journalPath() const {
        return path_ / "run-42.fxjr";
    }

private:
    std::filesystem::path path_{};
};

std::vector<std::byte> readBytes(const std::filesystem::path &path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    EXPECT_TRUE(stream.is_open());
    const auto size = stream.tellg();
    EXPECT_GE(size, 0);
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    stream.seekg(0);
    stream.read(reinterpret_cast<char *>(bytes.data()), size);
    EXPECT_TRUE(stream.good());
    return bytes;
}

ReplayedRunJournalV1 sentinelReplay() {
    return {
        .commandResults = {},
        .matchingState = std::make_unique<MatchingState>(domain::ExchangeRunId{999}),
    };
}

TEST(RunJournalReplayTest, EmptyPreparedJournalProducesUsableQualifiedMatchingState) {
    auto prepared = preparedJournal({});
    ReplayedRunJournalV1 replayed;

    const auto result = replayPreparedRunJournalV1(prepared, replayed);

    EXPECT_EQ(result.outcome, RunJournalReplayOutcome::REPLAYED);
    EXPECT_EQ(result.error, RunJournalReplayError::NONE);
    EXPECT_TRUE(replayed.commandResults.empty());
    ASSERT_NE(replayed.matchingState, nullptr);
    const MatchingStateSnapshot finalState = replayed.matchingState->snapshot();
    EXPECT_EQ(finalState.exchangeRunId, RUN_ID);
    EXPECT_TRUE(finalState.priceLevels.empty());
    EXPECT_TRUE(finalState.activeOrders.empty());

    const ImmutableCommandResultBatch continued =
        replayed.matchingState->processCommand(liveMessageFrom(replayOrder(1, domain::Side::BUY, 100, 1)));
    EXPECT_EQ(continued->correlation().exchangeRunId, RUN_ID);
    EXPECT_EQ(replayed.matchingState->snapshot().activeOrders.size(), 1U);
}

TEST(RunJournalReplayTest, ReconstructsOrderedNewFillCancelAndRejectionResultsAndFinalState) {
    std::vector<storage::JournalCommandV1> commands;
    commands.emplace_back(replayOrder(1, domain::Side::SELL, 100, 10));
    commands.emplace_back(replayOrder(2, domain::Side::SELL, 101, 5));
    commands.emplace_back(replayOrder(3, domain::Side::BUY, 100, 10, core::task::TimeInForce::IOC));
    commands.emplace_back(replayCancel(4, 2, domain::ClientId{102}));
    commands.emplace_back(replayCancel(5, 2, domain::ClientId{102}));
    auto prepared = preparedJournal(std::move(commands));
    ReplayedRunJournalV1 replayed;

    const auto result = replayPreparedRunJournalV1(prepared, replayed);

    EXPECT_EQ(result.outcome, RunJournalReplayOutcome::REPLAYED);
    ASSERT_EQ(replayed.commandResults.size(), 5U);
    for (std::size_t index = 0; index < replayed.commandResults.size(); ++index) {
        const auto &batch = replayed.commandResults[index];
        ASSERT_NE(batch, nullptr);
        EXPECT_EQ(batch->correlation().exchangeRunId, RUN_ID);
        EXPECT_EQ(batch->commandSequence(), domain::CommandSequence{index + 1});
        for (const domain::BusinessEvent &event : batch->events()) {
            EXPECT_EQ(std::visit([](const auto &typed) { return typed.eventId.exchangeRunId; }, event), RUN_ID);
        }
    }
    ASSERT_EQ(replayed.commandResults[2]->events().size(), 1U);
    EXPECT_NE(std::get_if<domain::Trade>(&replayed.commandResults[2]->events()[0]), nullptr);
    ASSERT_EQ(replayed.commandResults[3]->events().size(), 1U);
    EXPECT_NE(std::get_if<domain::OrderCancelled>(&replayed.commandResults[3]->events()[0]), nullptr);
    ASSERT_EQ(replayed.commandResults[4]->events().size(), 1U);
    const auto *rejection = std::get_if<domain::CommandRejected>(&replayed.commandResults[4]->events()[0]);
    ASSERT_NE(rejection, nullptr);
    EXPECT_EQ(rejection->reason, domain::CommandRejectionReason::ORDER_NOT_ACTIVE);
    ASSERT_NE(replayed.matchingState, nullptr);
    const MatchingStateSnapshot finalState = replayed.matchingState->snapshot();
    EXPECT_TRUE(finalState.priceLevels.empty());
    EXPECT_TRUE(finalState.activeOrders.empty());
}

TEST(RunJournalReplayTest, ReconstructsBookCapacityRejectionWithoutMutatingFullBook) {
    std::vector<storage::JournalCommandV1> commands;
    for (std::uint64_t sequence = 1; sequence <= 10; ++sequence) {
        commands.emplace_back(replayOrder(sequence, domain::Side::BUY, 100, 100'000'000));
    }
    commands.emplace_back(replayOrder(11, domain::Side::BUY, 100, 1));
    auto prepared = preparedJournal(std::move(commands));
    ReplayedRunJournalV1 replayed;

    const auto result = replayPreparedRunJournalV1(prepared, replayed);

    EXPECT_EQ(result.outcome, RunJournalReplayOutcome::REPLAYED);
    ASSERT_EQ(replayed.commandResults.size(), 11U);
    EXPECT_EQ(replayed.commandResults.back()->result(), ProcessingResult::BOOK_CAPACITY_EXCEEDED);
    ASSERT_EQ(replayed.commandResults.back()->events().size(), 1U);
    EXPECT_NE(std::get_if<domain::CommandRejected>(&replayed.commandResults.back()->events()[0]), nullptr);
    ASSERT_NE(replayed.matchingState, nullptr);
    const MatchingStateSnapshot finalState = replayed.matchingState->snapshot();
    ASSERT_EQ(finalState.priceLevels.size(), 1U);
    EXPECT_EQ(finalState.priceLevels[0].totalQuantity, domain::Quantity{1'000'000'000});
    EXPECT_EQ(finalState.priceLevels[0].orders.size(), 10U);
    EXPECT_EQ(finalState.activeOrders.size(), 10U);
}

TEST(RunJournalReplayTest, IsRepeatableAndDoesNotMutatePreparedJournal) {
    ReplayTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const auto first = replayOrder(1, domain::Side::BUY, 100, 10);
    const auto second = replayCancel(2, 1, first.clientId);
    std::unique_ptr<storage::RunJournalWriterV1> createdWriter;
    ASSERT_EQ(storage::createRunJournalWriterV1(directory.journalPath(), replayHeader(), createdWriter).outcome,
              storage::RunJournalCreateOutcome::CREATED);
    ASSERT_EQ(createdWriter->append(first).outcome, storage::RunJournalAppendOutcome::COMMITTED);
    ASSERT_EQ(createdWriter->append(second).outcome, storage::RunJournalAppendOutcome::COMMITTED);
    createdWriter.reset();
    storage::PreparedRunJournalV1 prepared;
    ASSERT_EQ(storage::prepareRunJournalV1(directory.journalPath(), prepared).outcome,
              storage::RunJournalRecoveryOutcome::PREPARED);
    const auto originalJournal = prepared.journal;
    const auto originalBytes = readBytes(directory.journalPath());
    const auto originalCommandCount = prepared.writer->committedCommandCount();
    const auto originalByteCount = prepared.writer->committedByteCount();
    ReplayedRunJournalV1 firstReplay;
    ReplayedRunJournalV1 secondReplay;

    EXPECT_EQ(replayPreparedRunJournalV1(prepared, firstReplay).outcome, RunJournalReplayOutcome::REPLAYED);
    EXPECT_EQ(replayPreparedRunJournalV1(prepared, secondReplay).outcome, RunJournalReplayOutcome::REPLAYED);

    expectEqualResults(firstReplay.commandResults, secondReplay.commandResults);
    ASSERT_NE(firstReplay.matchingState, nullptr);
    ASSERT_NE(secondReplay.matchingState, nullptr);
    EXPECT_NE(firstReplay.matchingState.get(), secondReplay.matchingState.get());
    EXPECT_EQ(firstReplay.matchingState->snapshot(), secondReplay.matchingState->snapshot());
    EXPECT_EQ(prepared.journal, originalJournal);
    EXPECT_EQ(prepared.writer->committedCommandCount(), originalCommandCount);
    EXPECT_EQ(prepared.writer->committedByteCount(), originalByteCount);
    EXPECT_EQ(readBytes(directory.journalPath()), originalBytes);
}

TEST(RunJournalReplayTest, ProducesTheSameResultsAndStateAsTheLiveSynchronousTransition) {
    std::vector<storage::JournalCommandV1> commands;
    commands.emplace_back(replayOrder(1, domain::Side::SELL, 100, 10));
    commands.emplace_back(replayOrder(2, domain::Side::BUY, 100, 4, core::task::TimeInForce::IOC));
    commands.emplace_back(replayCancel(3, 1, domain::ClientId{101}));
    auto prepared = preparedJournal(commands);
    ReplayedRunJournalV1 replayed;
    ASSERT_EQ(replayPreparedRunJournalV1(prepared, replayed).outcome, RunJournalReplayOutcome::REPLAYED);

    core::SharedQueue<sequencer::sequenceMessage> commandQueue(8);
    core::Bus bus(8);
    BoundedCommandResultQueue resultQueue(8);
    MatchingState liveState(RUN_ID);
    LiveMatchingHarness live(commandQueue, bus, resultQueue, liveState);
    for (const storage::JournalCommandV1 &command : commands) {
        ASSERT_TRUE(std::visit([&](const auto &typed) { return commandQueue.push(liveMessageFrom(typed)); }, command));
    }
    ASSERT_TRUE(live.drain());
    std::vector<ImmutableCommandResultBatch> liveResults;
    ImmutableCommandResultBatch batch;
    while (resultQueue.tryPop(batch)) {
        liveResults.push_back(std::move(batch));
    }

    expectEqualResults(replayed.commandResults, liveResults);
    ASSERT_NE(replayed.matchingState, nullptr);
    EXPECT_EQ(replayed.matchingState->snapshot(), liveState.snapshot());
}

TEST(RunJournalReplayTest, SequenceContextAndMatchingFailuresPreserveOutput) {
    const auto expectFailure = [](storage::PreparedRunJournalV1 prepared, const RunJournalReplayOutcome outcome,
                                  const RunJournalReplayError error) {
        auto output = sentinelReplay();
        MatchingState *const originalState = output.matchingState.get();
        const MatchingStateSnapshot originalSnapshot = output.matchingState->snapshot();
        const auto result = replayPreparedRunJournalV1(prepared, output);
        EXPECT_EQ(result.outcome, outcome);
        EXPECT_EQ(result.error, error);
        EXPECT_TRUE(output.commandResults.empty());
        EXPECT_EQ(output.matchingState.get(), originalState);
        EXPECT_EQ(output.matchingState->snapshot(), originalSnapshot);
    };

    auto gap = replayOrder(2, domain::Side::BUY, 100, 1);
    expectFailure(preparedJournal({gap}), RunJournalReplayOutcome::INVALID_JOURNAL,
                  RunJournalReplayError::COMMAND_SEQUENCE_MISMATCH);

    auto wrongRun = replayOrder(1, domain::Side::BUY, 100, 1);
    wrongRun.exchangeRunId = domain::ExchangeRunId{43};
    expectFailure(preparedJournal({wrongRun}), RunJournalReplayOutcome::INVALID_JOURNAL,
                  RunJournalReplayError::EXCHANGE_RUN_ID_MISMATCH);

    auto wrongConfiguration = replayOrder(1, domain::Side::BUY, 100, 1);
    wrongConfiguration.configurationVersion = 2;
    expectFailure(preparedJournal({wrongConfiguration}), RunJournalReplayOutcome::INVALID_JOURNAL,
                  RunJournalReplayError::INSTRUMENT_CONFIGURATION_MISMATCH);

    auto wrongRules = replayOrder(1, domain::Side::BUY, 100, 1);
    wrongRules.behavioralRulesVersion = 2;
    expectFailure(preparedJournal({wrongRules}), RunJournalReplayOutcome::INVALID_JOURNAL,
                  RunJournalReplayError::BEHAVIORAL_RULES_VERSION_MISMATCH);

    std::vector<storage::JournalCommandV1> wrongRoute;
    wrongRoute.emplace_back(replayOrder(1, domain::Side::BUY, 100, 1));
    wrongRoute.emplace_back(replayCancel(2, 1, domain::ClientId{101}, domain::InstrumentId{2}));
    expectFailure(preparedJournal(std::move(wrongRoute), true), RunJournalReplayOutcome::INVARIANT_FAILURE,
                  RunJournalReplayError::MATCHING_INVARIANT_FAILURE);

    auto invalidTimeInForce = replayOrder(1, domain::Side::BUY, 100, 1);
    invalidTimeInForce.timeInForce = static_cast<core::task::TimeInForce>(255);
    expectFailure(preparedJournal({invalidTimeInForce}), RunJournalReplayOutcome::INVARIANT_FAILURE,
                  RunJournalReplayError::MATCHING_INVARIANT_FAILURE);
}

} // namespace
} // namespace exchange::matching_engine
