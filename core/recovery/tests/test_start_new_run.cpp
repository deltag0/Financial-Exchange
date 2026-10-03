#include "start_new_run.hpp"
#include "../../admission/tests/admission_test_access.hpp"

#include "start_new_run_internal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>

namespace exchange::core {
namespace {

class StartNewRunTestDirectory final {
public:
    StartNewRunTestDirectory() {
        std::array<char, 45> pathTemplate{};
        constexpr char TEMPLATE[] = "/tmp/exchange-start-run-test-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), pathTemplate.begin());
        char *created = ::mkdtemp(pathTemplate.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~StartNewRunTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    StartNewRunTestDirectory(const StartNewRunTestDirectory &) = delete;
    StartNewRunTestDirectory &operator=(const StartNewRunTestDirectory &) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return !path_.empty();
    }

    [[nodiscard]] std::filesystem::path catalogPath() const {
        return path_ / "run-catalog-v1";
    }

    [[nodiscard]] std::filesystem::path journalPath(const std::uint64_t runId) const {
        return path_ / ("run-" + std::to_string(runId) + ".fxjr");
    }

private:
    std::filesystem::path path_{};
};

storage::NewRunConfigurationV1 startConfiguration() {
    return {
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = 3,
        .maxRunJournalBytes = 64 * 1024,
        .instruments = {{domain::InstrumentId{1}, 1}},
    };
}

storage::RunHeaderV1 sentinelHeader() {
    return {
        .exchangeRunId = domain::ExchangeRunId{999},
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = 2,
        .maxRunJournalBytes = 64 * 1024,
        .instruments = {{domain::InstrumentId{1}, 1}},
    };
}

struct RunStatePointers final {
    storage::RunJournalWriterV1 *journalWriter{nullptr};
    matching_engine::MatchingState *matchingState{nullptr};
    admission::CommandAdmissionIndex *admissionIndex{nullptr};
};

RunStatePointers installStartSentinel(StartNewRunTestDirectory &directory, RunStateV1 &output) {
    std::unique_ptr<storage::RunJournalWriterV1> writer;
    EXPECT_EQ(storage::createRunJournalWriterV1(directory.journalPath(999), sentinelHeader(), writer).outcome,
              storage::RunJournalCreateOutcome::CREATED);
    output = {
        .journalWriter = std::move(writer),
        .matchingState = std::make_unique<matching_engine::MatchingState>(domain::ExchangeRunId{999}),
        .admissionIndex = std::make_unique<admission::CommandAdmissionIndex>(2, domain::ExchangeRunId{999}),
    };
    return {output.journalWriter.get(), output.matchingState.get(), output.admissionIndex.get()};
}

void expectStartOutputUnchanged(const RunStateV1 &output, const RunStatePointers pointers) {
    EXPECT_EQ(output.journalWriter.get(), pointers.journalWriter);
    EXPECT_EQ(output.matchingState.get(), pointers.matchingState);
    EXPECT_EQ(output.admissionIndex.get(), pointers.admissionIndex);
    ASSERT_NE(output.matchingState, nullptr);
    ASSERT_NE(output.admissionIndex, nullptr);
    EXPECT_EQ(output.matchingState->snapshot().exchangeRunId, domain::ExchangeRunId{999});
    EXPECT_EQ(output.admissionIndex->capacity(), 2U);
}

struct StartScript final {
    std::optional<StartNewRunOutcome> construction{};
    std::optional<storage::NewRunActivationResult> activationResult{};
    int constructionCalls{0};
    int activationCalls{0};
};

std::optional<StartNewRunOutcome> scriptedConstruction(
    void *context, const domain::ExchangeRunId exchangeRunId, const std::uint64_t maxRunCommands,
    std::unique_ptr<matching_engine::MatchingState> &matchingState,
    std::unique_ptr<admission::CommandAdmissionIndex> &admissionIndex) noexcept {
    auto &script = *static_cast<StartScript *>(context);
    ++script.constructionCalls;
    if (script.construction.has_value()) {
        return script.construction;
    }
    return detail::constructEmptyRunStateV1(exchangeRunId, maxRunCommands, matchingState, admissionIndex);
}

storage::NewRunActivationResult scriptedActivation(void *context, const std::filesystem::path &canonicalCatalogPath,
                                                   const storage::PreparedNewRunJournalV1 &prepared) noexcept {
    auto &script = *static_cast<StartScript *>(context);
    ++script.activationCalls;
    if (script.activationResult.has_value()) {
        return *script.activationResult;
    }
    return storage::activatePreparedNewRunV1(canonicalCatalogPath, prepared);
}

detail::StartNewRunHooks hooksFor(StartScript &script) {
    return {&script, scriptedConstruction, scriptedActivation};
}

storage::RunCatalogSnapshotV1 loadCatalog(const std::filesystem::path &path) {
    storage::RunCatalogSnapshotV1 catalog{};
    EXPECT_EQ(storage::loadRunCatalogV1(path, catalog).outcome, storage::RunCatalogLoadOutcome::LOADED);
    return catalog;
}

sequencer::sequenceMessage firstCommand() {
    sequencer::sequenceMessage command{};
    command.orderId = domain::OrderId{1};
    command.globalSequenceNumber = domain::CommandSequence{1};
    command.clientId = domain::ClientId{11};
    command.clientCommandId = domain::ClientCommandId{"START-1"};
    command.instrumentId = domain::InstrumentId{1};
    command.configurationVersion = 1;
    command.price = domain::Price{1'000};
    command.quantity = domain::Quantity{5};
    command.type = sequencer::orderType::BUY;
    command.tif = task::TimeInForce::GTC;
    return command;
}

storage::JournalNewOrderV1 firstJournalCommand() {
    return {
        .exchangeRunId = domain::ExchangeRunId{1},
        .commandSequence = domain::CommandSequence{1},
        .behavioralRulesVersion = 1,
        .configurationVersion = 1,
        .clientId = domain::ClientId{11},
        .instrumentId = domain::InstrumentId{1},
        .clientCommandId = domain::ClientCommandId{"START-1"},
        .side = domain::Side::BUY,
        .timeInForce = task::TimeInForce::GTC,
        .price = domain::Price{1'000},
        .quantity = domain::Quantity{5},
    };
}

TEST(StartNewRunTest, ReturnsThreeEmptyOwnersForOneDurablyOpenRun) {
    StartNewRunTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    RunStateV1 started;

    const StartNewRunResult result = startNewRunV1(directory.catalogPath(), startConfiguration(), started);

    EXPECT_EQ(result.outcome, StartNewRunOutcome::STARTED);
    EXPECT_EQ(result.preparation.outcome, storage::NewRunJournalPreparationOutcome::PREPARED);
    ASSERT_TRUE(result.activation.has_value());
    EXPECT_EQ(result.activation->outcome, storage::NewRunActivationOutcome::ACTIVATED);
    ASSERT_NE(started.journalWriter, nullptr);
    ASSERT_NE(started.matchingState, nullptr);
    ASSERT_NE(started.admissionIndex, nullptr);
    EXPECT_EQ(started.journalWriter->committedCommandCount(), 0U);
    EXPECT_EQ(started.journalWriter->nextCommandSequence(), domain::CommandSequence{1});
    const matching_engine::MatchingStateSnapshot matching = started.matchingState->snapshot();
    EXPECT_EQ(matching.exchangeRunId, domain::ExchangeRunId{1});
    EXPECT_TRUE(matching.priceLevels.empty());
    EXPECT_TRUE(matching.activeOrders.empty());
    EXPECT_EQ(started.admissionIndex->capacity(), startConfiguration().maxRunCommands);
    EXPECT_EQ(started.admissionIndex->size(), 0U);
    EXPECT_TRUE(started.admissionIndex->statistics() == admission::AdmissionStatistics{});

    const storage::RunCatalogSnapshotV1 catalog = loadCatalog(directory.catalogPath());
    EXPECT_EQ(catalog.generation, 2U);
    EXPECT_EQ(catalog.lastReservedRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(catalog.activeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(catalog.activeDisposition, storage::RunCatalogDisposition::OPEN);
    EXPECT_TRUE(std::filesystem::exists(directory.journalPath(1)));
}

TEST(StartNewRunTest, ProcessesOneCommandThroughAdmissionJournalMatchingAndCompletion) {
    StartNewRunTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    RunStateV1 started;
    ASSERT_EQ(startNewRunV1(directory.catalogPath(), startConfiguration(), started).outcome,
              StartNewRunOutcome::STARTED);
    sequencer::sequenceMessage command = firstCommand();

    EXPECT_EQ(started.admissionIndex->reserve(command).status, admission::AdmissionStatus::ADMISSION_UNAVAILABLE);
    // Isolated composition returns a closed index; the controller owns production opening.
    admission::CommandAdmissionIndexTestAccess::setAdmissionOpen(*started.admissionIndex, true);
    EXPECT_EQ(started.admissionIndex->reserve(command).status, admission::AdmissionStatus::FIRST_SUBMISSION);
    EXPECT_EQ(started.journalWriter->append(firstJournalCommand()).outcome,
              storage::RunJournalAppendOutcome::COMMITTED);
    EXPECT_EQ(started.admissionIndex->markSequenced(command), admission::MarkSequencedStatus::SEQUENCED);
    const matching_engine::ImmutableCommandResultBatch result = started.matchingState->processCommand(command);
    ASSERT_NE(result, nullptr);
    EXPECT_EQ(result->correlation().exchangeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(result->correlation().commandSequence, domain::CommandSequence{1});
    EXPECT_EQ(started.admissionIndex->complete(result), admission::CompletionStatus::COMPLETED);
    EXPECT_EQ(started.admissionIndex->completedResult(command), result);
    EXPECT_EQ(started.journalWriter->committedCommandCount(), 1U);
    EXPECT_EQ(started.journalWriter->nextCommandSequence(), domain::CommandSequence{2});

    const matching_engine::MatchingStateSnapshot matching = started.matchingState->snapshot();
    EXPECT_EQ(matching.exchangeRunId, domain::ExchangeRunId{1});
    ASSERT_EQ(matching.activeOrders.size(), 1U);
    EXPECT_EQ(matching.activeOrders[0].orderId, domain::OrderId{1});
    EXPECT_EQ(matching.activeOrders[0].remainingQuantity, domain::Quantity{5});
}

TEST(StartNewRunTest, InvalidConfigurationSkipsStateConstructionAndActivationAndPreservesOutput) {
    StartNewRunTestDirectory sentinelDirectory;
    StartNewRunTestDirectory targetDirectory;
    ASSERT_TRUE(sentinelDirectory.valid());
    ASSERT_TRUE(targetDirectory.valid());
    RunStateV1 output;
    const RunStatePointers original = installStartSentinel(sentinelDirectory, output);
    storage::NewRunConfigurationV1 invalid = startConfiguration();
    invalid.maxRunCommands = 0;
    StartScript script;
    const detail::StartNewRunHooks hooks = hooksFor(script);

    const StartNewRunResult result =
        detail::startNewRunV1WithHooks(targetDirectory.catalogPath(), invalid, hooks, output);

    EXPECT_EQ(result.outcome, StartNewRunOutcome::PREPARATION_FAILED);
    EXPECT_EQ(result.preparation.outcome, storage::NewRunJournalPreparationOutcome::HEADER_VALIDATION_FAILED);
    EXPECT_EQ(result.preparation.headerValidation, storage::RunHeaderCodecError::ZERO_MAX_RUN_COMMANDS);
    EXPECT_FALSE(result.activation.has_value());
    EXPECT_EQ(script.constructionCalls, 0);
    EXPECT_EQ(script.activationCalls, 0);
    EXPECT_FALSE(std::filesystem::exists(targetDirectory.catalogPath()));
    EXPECT_FALSE(std::filesystem::exists(targetDirectory.journalPath(1)));
    expectStartOutputUnchanged(output, original);
}

TEST(StartNewRunTest, ConstructionFailuresPreserveEvidenceSkipActivationAndPreserveOutput) {
    const auto runCase = [](const StartNewRunOutcome construction) {
        StartNewRunTestDirectory sentinelDirectory;
        StartNewRunTestDirectory targetDirectory;
        ASSERT_TRUE(sentinelDirectory.valid());
        ASSERT_TRUE(targetDirectory.valid());
        RunStateV1 output;
        const RunStatePointers original = installStartSentinel(sentinelDirectory, output);
        StartScript script{.construction = construction};
        const detail::StartNewRunHooks hooks = hooksFor(script);

        const StartNewRunResult result =
            detail::startNewRunV1WithHooks(targetDirectory.catalogPath(), startConfiguration(), hooks, output);

        EXPECT_EQ(result.outcome, construction);
        EXPECT_EQ(result.preparation.outcome, storage::NewRunJournalPreparationOutcome::PREPARED);
        EXPECT_FALSE(result.activation.has_value());
        EXPECT_EQ(script.constructionCalls, 1);
        EXPECT_EQ(script.activationCalls, 0);
        const storage::RunCatalogSnapshotV1 catalog = loadCatalog(targetDirectory.catalogPath());
        EXPECT_EQ(catalog.lastReservedRunId, domain::ExchangeRunId{1});
        EXPECT_FALSE(catalog.activeRunId.has_value());
        EXPECT_EQ(catalog.activeDisposition, storage::RunCatalogDisposition::NONE);
        EXPECT_TRUE(std::filesystem::exists(targetDirectory.journalPath(1)));
        expectStartOutputUnchanged(output, original);
    };

    runCase(StartNewRunOutcome::CAPACITY_UNREPRESENTABLE);
    runCase(StartNewRunOutcome::ALLOCATION_FAILURE);
    runCase(StartNewRunOutcome::STATE_CONSTRUCTION_FAILED);
}

TEST(StartNewRunTest, ActivationFailureAndUncertaintyPreserveOutputAndNestedEvidence) {
    const auto runCase = [](const storage::RunCatalogReplaceResult replacement) {
        StartNewRunTestDirectory sentinelDirectory;
        StartNewRunTestDirectory targetDirectory;
        ASSERT_TRUE(sentinelDirectory.valid());
        ASSERT_TRUE(targetDirectory.valid());
        RunStateV1 output;
        const RunStatePointers original = installStartSentinel(sentinelDirectory, output);
        StartScript script{
            .activationResult =
                storage::NewRunActivationResult{
                    .outcome = storage::NewRunActivationOutcome::CATALOG_REPLACE_FAILED,
                    .catalogLoad = storage::RunCatalogLoadResult{storage::RunCatalogLoadOutcome::LOADED},
                    .catalogReplacement = replacement,
                },
        };
        const detail::StartNewRunHooks hooks = hooksFor(script);

        const StartNewRunResult result =
            detail::startNewRunV1WithHooks(targetDirectory.catalogPath(), startConfiguration(), hooks, output);

        EXPECT_EQ(result.outcome, StartNewRunOutcome::ACTIVATION_FAILED);
        EXPECT_EQ(result.preparation.outcome, storage::NewRunJournalPreparationOutcome::PREPARED);
        ASSERT_TRUE(result.activation.has_value());
        EXPECT_EQ(result.activation->outcome, storage::NewRunActivationOutcome::CATALOG_REPLACE_FAILED);
        ASSERT_TRUE(result.activation->catalogReplacement.has_value());
        EXPECT_EQ(result.activation->catalogReplacement->outcome, replacement.outcome);
        EXPECT_EQ(result.activation->catalogReplacement->systemError, replacement.systemError);
        EXPECT_EQ(script.constructionCalls, 1);
        EXPECT_EQ(script.activationCalls, 1);
        const storage::RunCatalogSnapshotV1 catalog = loadCatalog(targetDirectory.catalogPath());
        EXPECT_EQ(catalog.lastReservedRunId, domain::ExchangeRunId{1});
        if (replacement.outcome != storage::RunCatalogReplaceOutcome::UNCERTAIN) {
            EXPECT_FALSE(catalog.activeRunId.has_value());
        }
        EXPECT_TRUE(std::filesystem::exists(targetDirectory.journalPath(1)));
        expectStartOutputUnchanged(output, original);
    };

    runCase({storage::RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE, storage::RunCatalogCodecError::NONE, EIO});
    runCase({storage::RunCatalogReplaceOutcome::UNCERTAIN, storage::RunCatalogCodecError::NONE, EIO});
}

} // namespace
} // namespace exchange::core
