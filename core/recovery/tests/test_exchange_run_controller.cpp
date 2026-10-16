#include "exchange_run_controller.hpp"
#include "../../admission/tests/admission_test_access.hpp"

#include "run_catalog_storage_internal.hpp"
#include "new_run_activation_internal.hpp"
#include "start_new_run_internal.hpp"
#include "shared_queue.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <vector>

namespace exchange::core {
namespace {

constexpr domain::ExchangeRunId RUN_ID{42};

static_assert(!std::is_move_constructible_v<ExchangeRunController>);
static_assert(!std::is_move_assignable_v<ExchangeRunController>);

class StartupDirectory final {
public:
    StartupDirectory() {
        std::array<char, 64> buffer{};
        constexpr char TEMPLATE[] = "/tmp/exchange-existing-startup-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), buffer.begin());
        const char *created = ::mkdtemp(buffer.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~StartupDirectory() {
        if (!path_.empty()) {
            std::error_code error;
            std::filesystem::remove_all(path_, error);
        }
    }

    bool valid() const noexcept {
        return !path_.empty();
    }
    std::filesystem::path catalog() const {
        return path_ / "run-catalog-v1";
    }
    std::filesystem::path journal(const std::uint64_t id = 42) const {
        return path_ / ("run-" + std::to_string(id) + ".fxjr");
    }

private:
    std::filesystem::path path_{};
};

storage::RunHeaderV1 startupHeader(const domain::ExchangeRunId runId = RUN_ID) {
    return {
        .exchangeRunId = runId,
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = 10,
        .maxRunJournalBytes = 64 * 1024,
        .instruments = {{domain::InstrumentId{1}, 1}},
    };
}

storage::RunCatalogSnapshotV1 startupCatalog(
    const storage::RunCatalogDisposition disposition = storage::RunCatalogDisposition::OPEN) {
    return {
        .generation = 1,
        .lastReservedRunId = domain::ExchangeRunId{43},
        .activeRunId = RUN_ID,
        .activeDisposition = disposition,
        .retainedStoppedRunId = domain::ExchangeRunId{7},
    };
}

void createStartupJournal(const std::filesystem::path &path, const domain::ExchangeRunId runId = RUN_ID,
                          const bool mixed = false) {
    std::unique_ptr<storage::RunJournalWriterV1> writer;
    ASSERT_EQ(storage::createRunJournalWriterV1(path, startupHeader(runId), writer).outcome,
              storage::RunJournalCreateOutcome::CREATED);
    if (!mixed) {
        return;
    }
    storage::JournalNewOrderV1 sell{
        .exchangeRunId = runId,
        .commandSequence = domain::CommandSequence{1},
        .behavioralRulesVersion = 1,
        .configurationVersion = 1,
        .clientId = domain::ClientId{11},
        .instrumentId = domain::InstrumentId{1},
        .clientCommandId = domain::ClientCommandId{"SELL"},
        .side = domain::Side::SELL,
        .timeInForce = task::TimeInForce::GTC,
        .price = domain::Price{100},
        .quantity = domain::Quantity{10},
    };
    ASSERT_EQ(writer->append(sell).outcome, storage::RunJournalAppendOutcome::COMMITTED);
    auto buy = sell;
    buy.commandSequence = domain::CommandSequence{2};
    buy.clientId = domain::ClientId{22};
    buy.clientCommandId = domain::ClientCommandId{"BUY"};
    buy.side = domain::Side::BUY;
    buy.timeInForce = task::TimeInForce::IOC;
    buy.quantity = domain::Quantity{4};
    ASSERT_EQ(writer->append(buy).outcome, storage::RunJournalAppendOutcome::COMMITTED);
    const storage::JournalCancelV1 cancel{
        .exchangeRunId = runId,
        .commandSequence = domain::CommandSequence{3},
        .behavioralRulesVersion = 1,
        .configurationVersion = 1,
        .clientId = domain::ClientId{11},
        .instrumentId = domain::InstrumentId{1},
        .clientCommandId = domain::ClientCommandId{"CANCEL"},
        .targetOrderId = domain::TargetOrderId{999},
    };
    ASSERT_EQ(writer->append(cancel).outcome, storage::RunJournalAppendOutcome::COMMITTED);
}

std::vector<char> startupBytes(const std::filesystem::path &path) {
    std::ifstream file(path, std::ios::binary);
    EXPECT_TRUE(file.is_open());
    return {std::istreambuf_iterator<char>{file}, std::istreambuf_iterator<char>{}};
}

void writeStartupBytes(const std::filesystem::path &path, const std::span<const char> bytes,
                       const bool append = false) {
    std::ofstream file(path, std::ios::binary | (append ? std::ios::app : std::ios::trunc));
    ASSERT_TRUE(file.is_open());
    file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(file.good());
}

void expectStartupUnavailable(const ExchangeRunController &controller,
                              const ExchangeRunStartupState state = ExchangeRunStartupState::UNAVAILABLE) {
    EXPECT_EQ(controller.state(), state);
    EXPECT_EQ(controller.matchingState(), nullptr);
    EXPECT_EQ(controller.admissionIndex(), nullptr);
}

sequencer::sequenceMessage sellLookup() {
    sequencer::sequenceMessage command{};
    command.clientId = domain::ClientId{11};
    command.clientCommandId = domain::ClientCommandId{"SELL"};
    command.instrumentId = domain::InstrumentId{1};
    command.configurationVersion = 1;
    command.type = sequencer::orderType::SELL;
    command.price = domain::Price{100};
    command.quantity = domain::Quantity{10};
    command.tif = task::TimeInForce::GTC;
    return command;
}

TEST(ExistingRunStartupTest, OpenAndPausedSelectOnlyActiveRunAndReconstructBeforeDurablePausedSuccess) {
    for (const auto disposition : {storage::RunCatalogDisposition::OPEN, storage::RunCatalogDisposition::PAUSED}) {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        createStartupJournal(directory.journal(), RUN_ID, true);
        createStartupJournal(directory.journal(7), domain::ExchangeRunId{7});
        createStartupJournal(directory.journal(43), domain::ExchangeRunId{43});
        auto expected = startupCatalog(disposition);
        ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), expected).outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        const auto originalJournal = startupBytes(directory.journal());
        ExchangeRunController controller;

        const auto result = controller.startupExistingRunV1(directory.catalog());

        EXPECT_EQ(result.outcome, ExistingRunStartupOutcome::PAUSED);
        ASSERT_TRUE(result.recovery.has_value());
        EXPECT_EQ(result.recovery->outcome, recovery::RecoveredRunOutcome::RECOVERED);
        ASSERT_TRUE(result.catalogReplacement.has_value());
        EXPECT_EQ(result.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
        EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
        ASSERT_NE(controller.matchingState(), nullptr);
        ASSERT_NE(controller.admissionIndex(), nullptr);
        EXPECT_FALSE(admission::CommandAdmissionIndexTestAccess::admissionOpen(*controller.admissionIndex()));
        const auto snapshot = controller.matchingState()->snapshot();
        EXPECT_EQ(snapshot.exchangeRunId, RUN_ID);
        ASSERT_EQ(snapshot.activeOrders.size(), 1U);
        EXPECT_EQ(snapshot.activeOrders[0].orderId, domain::OrderId{1});
        EXPECT_EQ(snapshot.activeOrders[0].remainingQuantity, domain::Quantity{6});
        EXPECT_TRUE(controller.matchingState()->invariantsHold());
        EXPECT_EQ(controller.admissionIndex()->size(), 3U);
        EXPECT_EQ(controller.admissionIndex()->capacity(), 10U);
        EXPECT_EQ(controller.admissionIndex()->statistics(), admission::AdmissionStatistics{});
        const auto completed = controller.admissionIndex()->completedResult(sellLookup());
        ASSERT_NE(completed, nullptr);
        EXPECT_EQ(completed->correlation().exchangeRunId, RUN_ID);
        EXPECT_EQ(completed->commandSequence(), domain::CommandSequence{1});
        EXPECT_EQ(controller.admissionIndex()->completedResult(sellLookup()), completed);
        ++expected.generation;
        expected.activeDisposition = storage::RunCatalogDisposition::PAUSED;
        storage::RunCatalogSnapshotV1 actual;
        ASSERT_EQ(storage::loadRunCatalogV1(directory.catalog(), actual).outcome,
                  storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(actual, expected);
        EXPECT_EQ(startupBytes(directory.journal()), originalJournal);
    }
}

TEST(ExistingRunStartupTest, HeaderOnlyRunStartsWithEmptyPausedState) {
    StartupDirectory directory;
    ASSERT_TRUE(directory.valid());
    createStartupJournal(directory.journal());
    ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog()).outcome,
              storage::RunCatalogReplaceOutcome::COMMITTED);
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupExistingRunV1(directory.catalog()).outcome, ExistingRunStartupOutcome::PAUSED);
    ASSERT_NE(controller.matchingState(), nullptr);
    EXPECT_TRUE(controller.matchingState()->snapshot().activeOrders.empty());
    ASSERT_NE(controller.admissionIndex(), nullptr);
    EXPECT_EQ(controller.admissionIndex()->size(), 0U);
}

TEST(ExistingRunStartupTest, RepairsOnlySelectedMatchingRunTailBeforePersistingPaused) {
    StartupDirectory directory;
    ASSERT_TRUE(directory.valid());
    createStartupJournal(directory.journal(), RUN_ID, true);
    const auto original = startupBytes(directory.journal());
    const std::array<char, 4> tail{'F', 'X', 'J', 'R'};
    writeStartupBytes(directory.journal(), tail, true);
    ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog()).outcome,
              storage::RunCatalogReplaceOutcome::COMMITTED);
    ExchangeRunController controller;

    const auto result = controller.startupExistingRunV1(directory.catalog());

    ASSERT_EQ(result.outcome, ExistingRunStartupOutcome::PAUSED);
    ASSERT_TRUE(result.recovery.has_value());
    const auto &preparation = result.recovery->preparation;
    EXPECT_EQ(preparation.validation.outcome, storage::RunJournalLoadOutcome::INCOMPLETE_TAIL);
    ASSERT_EQ(preparation.preservedTailBytes.size(), tail.size());
    EXPECT_TRUE(std::equal(tail.begin(), tail.end(), preparation.preservedTailBytes.begin(),
                           [](const char a, const std::byte b) { return static_cast<std::byte>(a) == b; }));
    EXPECT_EQ(startupBytes(directory.journal()), original);
    ASSERT_NE(controller.admissionIndex(), nullptr);
    EXPECT_EQ(controller.admissionIndex()->size(), 3U);
}

TEST(ExistingRunStartupTest, WrongRunIdentityNeverRepairsOrModifiesJournalOrCatalog) {
    for (const bool incomplete : {false, true}) {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        createStartupJournal(directory.journal(), domain::ExchangeRunId{43});
        if (incomplete) {
            const std::array<char, 4> tail{'F', 'X', 'J', 'R'};
            writeStartupBytes(directory.journal(), tail, true);
        }
        ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog()).outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        const auto originalJournal = startupBytes(directory.journal());
        const auto originalCatalog = startupBytes(directory.catalog());
        ExchangeRunController controller;

        const auto result = controller.startupExistingRunV1(directory.catalog());

        EXPECT_EQ(result.outcome, ExistingRunStartupOutcome::RECOVERY_FAILED);
        ASSERT_TRUE(result.recovery.has_value());
        EXPECT_EQ(result.recovery->preparation.outcome, storage::RunJournalRecoveryOutcome::CORRUPTION);
        EXPECT_EQ(result.recovery->preparation.validation.error,
                  storage::RunJournalLoadError::EXCHANGE_RUN_ID_MISMATCH);
        EXPECT_FALSE(result.catalogReplacement.has_value());
        expectStartupUnavailable(controller, ExchangeRunStartupState::RECOVERY_FAILED);
        EXPECT_EQ(startupBytes(directory.journal()), originalJournal);
        EXPECT_EQ(startupBytes(directory.catalog()), originalCatalog);
    }
}

TEST(ExistingRunStartupTest, MissingInvalidAndUnreadableCatalogsNeverInferOrCreateRuns) {
    for (const auto expected : {storage::RunCatalogLoadOutcome::MISSING, storage::RunCatalogLoadOutcome::INVALID,
                                storage::RunCatalogLoadOutcome::IO_FAILURE}) {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        createStartupJournal(directory.journal());
        const auto originalJournal = startupBytes(directory.journal());
        if (expected == storage::RunCatalogLoadOutcome::INVALID) {
            const std::array<char, 4> invalid{'F', 'X', 'R', 'C'};
            writeStartupBytes(directory.catalog(), invalid);
        } else if (expected == storage::RunCatalogLoadOutcome::IO_FAILURE) {
            ASSERT_TRUE(std::filesystem::create_directory(directory.catalog()));
        }
        ExchangeRunController controller;
        const auto result = controller.startupExistingRunV1(directory.catalog());
        EXPECT_EQ(result.outcome, ExistingRunStartupOutcome::CATALOG_LOAD_FAILED);
        EXPECT_EQ(result.catalogLoad.outcome, expected);
        EXPECT_FALSE(result.recovery.has_value());
        EXPECT_FALSE(result.catalogReplacement.has_value());
        expectStartupUnavailable(controller);
        EXPECT_EQ(startupBytes(directory.journal()), originalJournal);
        EXPECT_FALSE(std::filesystem::exists(directory.journal(44)));
        if (expected == storage::RunCatalogLoadOutcome::MISSING) {
            EXPECT_FALSE(std::filesystem::exists(directory.catalog()));
        }
    }
}

TEST(ExistingRunStartupTest, NoActiveCatalogNeverSelectsJournalByFilename) {
    StartupDirectory directory;
    ASSERT_TRUE(directory.valid());
    createStartupJournal(directory.journal());
    auto catalog = startupCatalog();
    catalog.activeRunId.reset();
    catalog.activeDisposition = storage::RunCatalogDisposition::NONE;
    ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), catalog).outcome,
              storage::RunCatalogReplaceOutcome::COMMITTED);
    const auto original = startupBytes(directory.catalog());
    ExchangeRunController controller;
    const auto result = controller.startupExistingRunV1(directory.catalog());
    EXPECT_EQ(result.outcome, ExistingRunStartupOutcome::NO_ACTIVE_RUN);
    EXPECT_FALSE(result.recovery.has_value());
    expectStartupUnavailable(controller);
    EXPECT_EQ(startupBytes(directory.catalog()), original);
    EXPECT_FALSE(std::filesystem::exists(directory.journal(44)));
}

TEST(ExistingRunStartupTest, TerminalAndFailedDispositionsRequireExplicitOperationWithoutRepairOrDowngrade) {
    for (const auto disposition :
         {storage::RunCatalogDisposition::CAPACITY_REACHED, storage::RunCatalogDisposition::FAIL_STOPPED,
          storage::RunCatalogDisposition::RECOVERY_FAILED}) {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        createStartupJournal(directory.journal());
        const std::array<char, 4> tail{'F', 'X', 'J', 'R'};
        writeStartupBytes(directory.journal(), tail, true);
        ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog(disposition)).outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        const auto originalJournal = startupBytes(directory.journal());
        const auto originalCatalog = startupBytes(directory.catalog());
        ExchangeRunController controller;
        const auto result = controller.startupExistingRunV1(directory.catalog());
        EXPECT_EQ(result.outcome, ExistingRunStartupOutcome::EXPLICIT_OPERATION_REQUIRED);
        EXPECT_FALSE(result.recovery.has_value());
        EXPECT_FALSE(result.catalogReplacement.has_value());
        expectStartupUnavailable(controller);
        EXPECT_EQ(startupBytes(directory.catalog()), originalCatalog);
        EXPECT_EQ(startupBytes(directory.journal()), originalJournal);
    }
}

TEST(ExistingRunStartupTest, GenerationExhaustionFailsWithoutRecoveryOrMutation) {
    StartupDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto catalog = startupCatalog();
    catalog.generation = std::numeric_limits<std::uint64_t>::max();
    storage::RunCatalogV1Bytes bytes{};
    ASSERT_EQ(storage::encodeRunCatalogV1(catalog, bytes), storage::RunCatalogCodecError::NONE);
    writeStartupBytes(directory.catalog(), {reinterpret_cast<const char *>(bytes.data()), bytes.size()});
    createStartupJournal(directory.journal());
    const auto originalCatalog = startupBytes(directory.catalog());
    const auto originalJournal = startupBytes(directory.journal());
    ExchangeRunController controller;
    const auto result = controller.startupExistingRunV1(directory.catalog());
    EXPECT_EQ(result.outcome, ExistingRunStartupOutcome::GENERATION_EXHAUSTED);
    EXPECT_FALSE(result.recovery.has_value());
    expectStartupUnavailable(controller);
    EXPECT_EQ(startupBytes(directory.catalog()), originalCatalog);
    EXPECT_EQ(startupBytes(directory.journal()), originalJournal);
}

TEST(ExistingRunStartupTest, MissingUnreadableAndCorruptJournalsReportRecoveryFailedPreservingEvidence) {
    for (const auto expected :
         {storage::RunJournalRecoveryOutcome::MISSING, storage::RunJournalRecoveryOutcome::IO_FAILURE,
          storage::RunJournalRecoveryOutcome::CORRUPTION}) {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog()).outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        if (expected == storage::RunJournalRecoveryOutcome::CORRUPTION) {
            const std::array<char, 4> damaged{'F', 'X', 'J', 'R'};
            writeStartupBytes(directory.journal(), damaged);
        } else if (expected == storage::RunJournalRecoveryOutcome::IO_FAILURE) {
            ASSERT_TRUE(std::filesystem::create_directory(directory.journal()));
        }
        const auto originalCatalog = startupBytes(directory.catalog());
        ExchangeRunController controller;
        const auto result = controller.startupExistingRunV1(directory.catalog());
        EXPECT_EQ(result.outcome, ExistingRunStartupOutcome::RECOVERY_FAILED);
        ASSERT_TRUE(result.recovery.has_value());
        EXPECT_EQ(result.recovery->preparation.outcome, expected);
        EXPECT_FALSE(result.catalogReplacement.has_value());
        expectStartupUnavailable(controller, ExchangeRunStartupState::RECOVERY_FAILED);
        EXPECT_EQ(startupBytes(directory.catalog()), originalCatalog);
        if (expected == storage::RunJournalRecoveryOutcome::CORRUPTION) {
            EXPECT_EQ(startupBytes(directory.journal()), (std::vector<char>{'F', 'X', 'J', 'R'}));
        } else if (expected == storage::RunJournalRecoveryOutcome::MISSING) {
            EXPECT_FALSE(std::filesystem::exists(directory.journal()));
        }
    }
}

ssize_t failStartupCatalogWrite(void *, int, const void *, std::size_t) noexcept {
    errno = EIO;
    return -1;
}

struct StartupSyncScript final {
    const ExchangeRunController *controller;
    bool failDirectorySync{false};
    int calls{0};
    bool unavailableUntilSync{true};
    const admission::CommandAdmissionIndex *admissionIndex{nullptr};
};

int startupCatalogSync(void *context, const int descriptor) noexcept {
    auto &script = *static_cast<StartupSyncScript *>(context);
    ++script.calls;
    script.unavailableUntilSync =
        script.unavailableUntilSync && script.controller->state() == ExchangeRunStartupState::UNAVAILABLE &&
        script.controller->matchingState() == nullptr && script.controller->admissionIndex() == nullptr;
    if (script.admissionIndex != nullptr) {
        EXPECT_FALSE(admission::CommandAdmissionIndexTestAccess::admissionOpen(*script.admissionIndex));
    }
    if (script.calls == 2 && script.failDirectorySync) {
        errno = EIO;
        return -1;
    }
    return ::fsync(descriptor);
}

TEST(ExistingRunStartupTest, DefiniteCatalogWriteFailureLeavesControllerUnavailableAndEvidenceUnchanged) {
    StartupDirectory directory;
    ASSERT_TRUE(directory.valid());
    createStartupJournal(directory.journal(), RUN_ID, true);
    ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog()).outcome,
              storage::RunCatalogReplaceOutcome::COMMITTED);
    const auto originalJournal = startupBytes(directory.journal());
    ExchangeRunController controller;
    const auto originalCatalog = startupBytes(directory.catalog());
    auto hooks = storage::detail::systemRunCatalogStorageHooks();
    hooks.writeFile = failStartupCatalogWrite;

    const auto result = detail::startupExistingRunV1WithHooks(directory.catalog(), hooks, controller);

    EXPECT_EQ(result.outcome, ExistingRunStartupOutcome::CATALOG_REPLACE_FAILED);
    ASSERT_TRUE(result.recovery.has_value());
    EXPECT_EQ(result.recovery->outcome, recovery::RecoveredRunOutcome::RECOVERED);
    ASSERT_TRUE(result.catalogReplacement.has_value());
    EXPECT_EQ(result.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE);
    EXPECT_EQ(result.catalogReplacement->systemError, EIO);
    expectStartupUnavailable(controller);
    EXPECT_EQ(startupBytes(directory.catalog()), originalCatalog);
    EXPECT_EQ(startupBytes(directory.journal()), originalJournal);
}

TEST(ExistingRunStartupTest, PublishesPausedOwnersOnlyAfterBothCatalogDurabilityStepsAndNeverOnUncertainty) {
    for (const bool uncertain : {false, true}) {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        createStartupJournal(directory.journal(), RUN_ID, true);
        ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog()).outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        const auto originalJournal = startupBytes(directory.journal());
        ExchangeRunController controller;
        StartupSyncScript script{&controller, uncertain};
        auto hooks = storage::detail::systemRunCatalogStorageHooks();
        hooks.context = &script;
        hooks.syncFile = startupCatalogSync;

        const auto result = detail::startupExistingRunV1WithHooks(directory.catalog(), hooks, controller);

        EXPECT_EQ(script.calls, 2);
        EXPECT_TRUE(script.unavailableUntilSync);
        ASSERT_TRUE(result.catalogReplacement.has_value());
        storage::RunCatalogSnapshotV1 observed;
        ASSERT_EQ(storage::loadRunCatalogV1(directory.catalog(), observed).outcome,
                  storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(observed.activeDisposition, storage::RunCatalogDisposition::PAUSED);
        EXPECT_EQ(observed.generation, 2U);
        if (uncertain) {
            EXPECT_EQ(result.outcome, ExistingRunStartupOutcome::CATALOG_REPLACE_FAILED);
            EXPECT_EQ(result.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::UNCERTAIN);
            EXPECT_EQ(result.catalogReplacement->systemError, EIO);
            ASSERT_TRUE(result.catalogReplacement->observedSnapshot.has_value());
            EXPECT_EQ(*result.catalogReplacement->observedSnapshot, observed);
            expectStartupUnavailable(controller);
        } else {
            EXPECT_EQ(result.outcome, ExistingRunStartupOutcome::PAUSED);
            EXPECT_EQ(result.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
            EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
            EXPECT_NE(controller.matchingState(), nullptr);
            EXPECT_NE(controller.admissionIndex(), nullptr);
        }
        EXPECT_EQ(startupBytes(directory.journal()), originalJournal);
    }
}

storage::NewRunConfigurationV1 controllerConfiguration() {
    return {
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = 10,
        .maxRunJournalBytes = 64 * 1024,
        .instruments = {{domain::InstrumentId{1}, 1}},
    };
}

TEST(NewRunControllerStartupTest, StartsOneEmptyReadyRunOnlyAfterDurableOpenActivation) {
    StartupDirectory directory;
    ASSERT_TRUE(directory.valid());
    ExchangeRunController controller;

    const auto result = controller.startupNewRunV1(directory.catalog(), controllerConfiguration());

    ASSERT_EQ(result.outcome, NewRunStartupOutcome::READY);
    ASSERT_TRUE(result.catalogLoad.has_value());
    EXPECT_EQ(result.catalogLoad->outcome, storage::RunCatalogLoadOutcome::MISSING);
    ASSERT_TRUE(result.startup.has_value());
    EXPECT_EQ(result.startup->outcome, StartNewRunOutcome::STARTED);
    ASSERT_TRUE(result.startup->activation.has_value());
    EXPECT_EQ(result.startup->activation->outcome, storage::NewRunActivationOutcome::ACTIVATED);
    ASSERT_TRUE(result.startup->activation->catalogReplacement.has_value());
    EXPECT_EQ(result.startup->activation->catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::READY);
    ASSERT_NE(controller.matchingState(), nullptr);
    const auto snapshot = controller.matchingState()->snapshot();
    EXPECT_EQ(snapshot.exchangeRunId, domain::ExchangeRunId{1});
    EXPECT_TRUE(snapshot.activeOrders.empty());
    EXPECT_TRUE(snapshot.priceLevels.empty());
    ASSERT_NE(controller.admissionIndex(), nullptr);
    EXPECT_EQ(controller.admissionIndex()->size(), 0U);
    EXPECT_EQ(controller.admissionIndex()->capacity(), 10U);
    EXPECT_EQ(controller.admissionIndex()->statistics(), admission::AdmissionStatistics{});
    storage::LoadedRunJournalV1 journal;
    ASSERT_EQ(storage::loadRunJournalV1(directory.journal(1), journal).outcome, storage::RunJournalLoadOutcome::VALID);
    EXPECT_EQ(journal.header, startupHeader(domain::ExchangeRunId{1}));
    EXPECT_TRUE(journal.commands.empty());
    EXPECT_EQ(journal.validCommittedByteCount, std::filesystem::file_size(directory.journal(1)));
    storage::RunCatalogSnapshotV1 catalog;
    ASSERT_EQ(storage::loadRunCatalogV1(directory.catalog(), catalog).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(catalog.generation, 2U);
    EXPECT_EQ(catalog.lastReservedRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(catalog.activeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(catalog.activeDisposition, storage::RunCatalogDisposition::OPEN);
    EXPECT_FALSE(catalog.retainedStoppedRunId.has_value());
}

TEST(NewRunControllerStartupTest, BothStartupMethodsRefuseOwnedReadyOrPausedRunsWithoutReplacingOwners) {
    for (const bool paused : {false, true}) {
        StartupDirectory ownedDirectory;
        StartupDirectory newDirectory;
        ASSERT_TRUE(ownedDirectory.valid());
        ASSERT_TRUE(newDirectory.valid());
        ExchangeRunController controller;
        if (paused) {
            createStartupJournal(ownedDirectory.journal(), RUN_ID, true);
            ASSERT_EQ(storage::replaceRunCatalogV1(ownedDirectory.catalog(), startupCatalog()).outcome,
                      storage::RunCatalogReplaceOutcome::COMMITTED);
            ASSERT_EQ(controller.startupExistingRunV1(ownedDirectory.catalog()).outcome,
                      ExistingRunStartupOutcome::PAUSED);
        } else {
            ASSERT_EQ(controller.startupNewRunV1(ownedDirectory.catalog(), controllerConfiguration()).outcome,
                      NewRunStartupOutcome::READY);
        }
        const auto *matching = controller.matchingState();
        const auto *admission = controller.admissionIndex();
        ASSERT_NE(matching, nullptr);
        ASSERT_NE(admission, nullptr);
        const auto state = controller.state();
        const auto snapshot = matching->snapshot();
        const auto oldCatalog = startupBytes(ownedDirectory.catalog());
        const auto oldJournal = startupBytes(ownedDirectory.journal(paused ? 42 : 1));
        createStartupJournal(newDirectory.journal(99), domain::ExchangeRunId{99});
        auto selected = startupCatalog();
        selected.lastReservedRunId = domain::ExchangeRunId{99};
        selected.activeRunId = domain::ExchangeRunId{99};
        ASSERT_EQ(storage::replaceRunCatalogV1(newDirectory.catalog(), selected).outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        const auto selectedCatalog = startupBytes(newDirectory.catalog());
        const auto selectedJournal = startupBytes(newDirectory.journal(99));

        for (const bool newRun : {true, false}) {
            if (newRun) {
                const auto result = controller.startupNewRunV1(newDirectory.catalog(), controllerConfiguration());
                EXPECT_EQ(result.outcome, NewRunStartupOutcome::RUN_ALREADY_OWNED);
                EXPECT_FALSE(result.catalogLoad.has_value());
                EXPECT_FALSE(result.startup.has_value());
            } else {
                const auto result = controller.startupExistingRunV1(newDirectory.catalog());
                EXPECT_EQ(result.outcome, ExistingRunStartupOutcome::EXPLICIT_OPERATION_REQUIRED);
                EXPECT_FALSE(result.recovery.has_value());
                EXPECT_FALSE(result.catalogReplacement.has_value());
            }
            EXPECT_EQ(controller.state(), state);
            EXPECT_EQ(controller.matchingState(), matching);
            EXPECT_EQ(controller.admissionIndex(), admission);
            ASSERT_NE(controller.matchingState(), nullptr);
            EXPECT_EQ(controller.matchingState()->snapshot(), snapshot);
            EXPECT_EQ(startupBytes(ownedDirectory.catalog()), oldCatalog);
            EXPECT_EQ(startupBytes(ownedDirectory.journal(paused ? 42 : 1)), oldJournal);
            EXPECT_EQ(startupBytes(newDirectory.catalog()), selectedCatalog);
            EXPECT_EQ(startupBytes(newDirectory.journal(99)), selectedJournal);
            EXPECT_FALSE(std::filesystem::exists(newDirectory.journal(100)));
        }
    }
}

TEST(NewRunControllerStartupTest, EveryActiveCatalogDispositionRefusesBeforeReservationAndPreservesEvidence) {
    for (const auto disposition :
         {storage::RunCatalogDisposition::OPEN, storage::RunCatalogDisposition::PAUSED,
          storage::RunCatalogDisposition::CAPACITY_REACHED, storage::RunCatalogDisposition::FAIL_STOPPED,
          storage::RunCatalogDisposition::RECOVERY_FAILED}) {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        createStartupJournal(directory.journal());
        const std::array<char, 4> tail{'F', 'X', 'J', 'R'};
        writeStartupBytes(directory.journal(), tail, true);
        ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog(disposition)).outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        const auto catalog = startupBytes(directory.catalog());
        const auto journal = startupBytes(directory.journal());
        ExchangeRunController controller;

        const auto result = controller.startupNewRunV1(directory.catalog(), controllerConfiguration());

        EXPECT_EQ(result.outcome, NewRunStartupOutcome::ACTIVE_RUN_EXISTS);
        ASSERT_TRUE(result.catalogLoad.has_value());
        EXPECT_EQ(result.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_FALSE(result.startup.has_value());
        expectStartupUnavailable(controller);
        EXPECT_EQ(startupBytes(directory.catalog()), catalog);
        EXPECT_EQ(startupBytes(directory.journal()), journal);
        EXPECT_FALSE(std::filesystem::exists(directory.journal(44)));
    }
}

TEST(NewRunControllerStartupTest, PreservesRetainedSelectionAndJournalWhenStartingNextRun) {
    StartupDirectory directory;
    ASSERT_TRUE(directory.valid());
    createStartupJournal(directory.journal(7), domain::ExchangeRunId{7});
    const auto retained = startupBytes(directory.journal(7));
    auto expected = startupCatalog();
    expected.lastReservedRunId = RUN_ID;
    expected.activeRunId.reset();
    expected.activeDisposition = storage::RunCatalogDisposition::NONE;
    ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), expected).outcome,
              storage::RunCatalogReplaceOutcome::COMMITTED);
    ExchangeRunController controller;

    const auto result = controller.startupNewRunV1(directory.catalog(), controllerConfiguration());

    ASSERT_EQ(result.outcome, NewRunStartupOutcome::READY);
    expected.generation = 3;
    expected.lastReservedRunId = domain::ExchangeRunId{43};
    expected.activeRunId = domain::ExchangeRunId{43};
    expected.activeDisposition = storage::RunCatalogDisposition::OPEN;
    storage::RunCatalogSnapshotV1 actual;
    ASSERT_EQ(storage::loadRunCatalogV1(directory.catalog(), actual).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(startupBytes(directory.journal(7)), retained);
    storage::LoadedRunJournalV1 journal;
    ASSERT_EQ(storage::loadRunJournalV1(directory.journal(43), journal).outcome, storage::RunJournalLoadOutcome::VALID);
    EXPECT_EQ(journal.header, startupHeader(domain::ExchangeRunId{43}));
    EXPECT_TRUE(journal.commands.empty());
    ASSERT_NE(controller.matchingState(), nullptr);
    EXPECT_EQ(controller.matchingState()->snapshot().exchangeRunId, domain::ExchangeRunId{43});
}

TEST(NewRunControllerStartupTest, ReusesPreparationValidationAndCatalogJournalEvidenceSafeguards) {
    {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        auto invalid = controllerConfiguration();
        invalid.maxRunCommands = 0;
        ExchangeRunController controller;
        const auto result = controller.startupNewRunV1(directory.catalog(), invalid);
        EXPECT_EQ(result.outcome, NewRunStartupOutcome::START_FAILED);
        ASSERT_TRUE(result.startup.has_value());
        EXPECT_EQ(result.startup->outcome, StartNewRunOutcome::PREPARATION_FAILED);
        EXPECT_EQ(result.startup->preparation.outcome,
                  storage::NewRunJournalPreparationOutcome::HEADER_VALIDATION_FAILED);
        EXPECT_EQ(result.startup->preparation.headerValidation, storage::RunHeaderCodecError::ZERO_MAX_RUN_COMMANDS);
        EXPECT_FALSE(result.startup->activation.has_value());
        expectStartupUnavailable(controller);
        EXPECT_FALSE(std::filesystem::exists(directory.catalog()));
        EXPECT_FALSE(std::filesystem::exists(directory.journal(1)));
    }
    for (const auto loadOutcome : {storage::RunCatalogLoadOutcome::INVALID, storage::RunCatalogLoadOutcome::IO_FAILURE,
                                   storage::RunCatalogLoadOutcome::MISSING}) {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        const std::array<char, 4> invalid{'F', 'X', 'R', 'C'};
        std::vector<char> journal;
        if (loadOutcome == storage::RunCatalogLoadOutcome::INVALID) {
            writeStartupBytes(directory.catalog(), invalid);
        } else if (loadOutcome == storage::RunCatalogLoadOutcome::IO_FAILURE) {
            ASSERT_TRUE(std::filesystem::create_directory(directory.catalog()));
        } else {
            createStartupJournal(directory.journal());
            journal = startupBytes(directory.journal());
        }
        ExchangeRunController controller;

        const auto result = controller.startupNewRunV1(directory.catalog(), controllerConfiguration());

        EXPECT_EQ(result.outcome, NewRunStartupOutcome::START_FAILED);
        ASSERT_TRUE(result.catalogLoad.has_value());
        EXPECT_EQ(result.catalogLoad->outcome, loadOutcome);
        if (loadOutcome == storage::RunCatalogLoadOutcome::MISSING) {
            ASSERT_TRUE(result.startup.has_value());
            EXPECT_EQ(result.startup->outcome, StartNewRunOutcome::PREPARATION_FAILED);
            EXPECT_EQ(result.startup->preparation.outcome,
                      storage::NewRunJournalPreparationOutcome::RESERVATION_FAILED);
            ASSERT_TRUE(result.startup->preparation.reservation.has_value());
            const auto &reservation = *result.startup->preparation.reservation;
            EXPECT_EQ(reservation.catalogLoad.outcome, loadOutcome);
            EXPECT_EQ(reservation.outcome, storage::ExchangeRunIdReservationOutcome::JOURNAL_PRESENT_WITHOUT_CATALOG);
            EXPECT_FALSE(result.startup->activation.has_value());
        } else {
            EXPECT_FALSE(result.startup.has_value());
        }
        expectStartupUnavailable(controller);
        EXPECT_FALSE(std::filesystem::exists(directory.journal(1)));
        if (loadOutcome == storage::RunCatalogLoadOutcome::MISSING) {
            EXPECT_FALSE(std::filesystem::exists(directory.catalog()));
            EXPECT_EQ(startupBytes(directory.journal()), journal);
        } else if (loadOutcome == storage::RunCatalogLoadOutcome::INVALID) {
            EXPECT_EQ(result.catalogLoad->codecError, storage::RunCatalogCodecError::INVALID_SIZE);
            EXPECT_EQ(startupBytes(directory.catalog()), std::vector<char>(invalid.begin(), invalid.end()));
        } else {
            EXPECT_TRUE(std::filesystem::is_directory(directory.catalog()));
            EXPECT_EQ(result.catalogLoad->systemError, EISDIR);
        }
    }
}

TEST(NewRunControllerStartupTest, JournalCreationFailureRetainsConsumedReservationAndExistingEvidence) {
    StartupDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto catalog = startupCatalog();
    catalog.lastReservedRunId = RUN_ID;
    catalog.activeRunId.reset();
    catalog.activeDisposition = storage::RunCatalogDisposition::NONE;
    ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), catalog).outcome,
              storage::RunCatalogReplaceOutcome::COMMITTED);
    createStartupJournal(directory.journal(43), domain::ExchangeRunId{43});
    const auto evidence = startupBytes(directory.journal(43));
    ExchangeRunController controller;

    const auto result = controller.startupNewRunV1(directory.catalog(), controllerConfiguration());

    EXPECT_EQ(result.outcome, NewRunStartupOutcome::START_FAILED);
    ASSERT_TRUE(result.startup.has_value());
    EXPECT_EQ(result.startup->outcome, StartNewRunOutcome::PREPARATION_FAILED);
    EXPECT_EQ(result.startup->preparation.outcome, storage::NewRunJournalPreparationOutcome::JOURNAL_CREATION_FAILED);
    ASSERT_TRUE(result.startup->preparation.journalCreation.has_value());
    EXPECT_EQ(result.startup->preparation.journalCreation->outcome,
              storage::RunJournalCreateOutcome::PATH_ALREADY_EXISTS);
    EXPECT_EQ(result.startup->preparation.journalCreation->systemError, EEXIST);
    expectStartupUnavailable(controller);
    EXPECT_EQ(startupBytes(directory.journal(43)), evidence);
    ASSERT_EQ(storage::loadRunCatalogV1(directory.catalog(), catalog).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(catalog.generation, 2U);
    EXPECT_EQ(catalog.lastReservedRunId, domain::ExchangeRunId{43});
    EXPECT_FALSE(catalog.activeRunId.has_value());
}

struct NewControllerStartScript final {
    std::optional<StartNewRunOutcome> constructionFailure{};
    storage::detail::RunCatalogStorageHooks activationHooks{};
    int constructionCalls{0};
    int activationCalls{0};
    admission::CommandAdmissionIndex *admissionIndex{nullptr};
    StartupSyncScript *sync{nullptr};
    matching_engine::MatchingState *matchingState{nullptr};
    storage::RunJournalWriterV1 *writer{nullptr};
};

std::optional<StartNewRunOutcome> constructControllerRun(
    void *context, const domain::ExchangeRunId runId, const std::uint64_t capacity,
    std::unique_ptr<matching_engine::MatchingState> &matchingState,
    std::unique_ptr<admission::CommandAdmissionIndex> &admissionIndex) noexcept {
    auto &script = *static_cast<NewControllerStartScript *>(context);
    ++script.constructionCalls;
    if (script.constructionFailure.has_value()) {
        return script.constructionFailure;
    }
    const auto failure = detail::constructEmptyRunStateV1(runId, capacity, matchingState, admissionIndex);
    script.admissionIndex = admissionIndex.get();
    script.matchingState = matchingState.get();
    if (script.sync != nullptr) {
        script.sync->admissionIndex = admissionIndex.get();
    }
    return failure;
}

storage::NewRunActivationResult activateControllerRun(void *context, const std::filesystem::path &catalogPath,
                                                      const storage::PreparedNewRunJournalV1 &prepared) noexcept {
    auto &script = *static_cast<NewControllerStartScript *>(context);
    ++script.activationCalls;
    script.writer = prepared.writer.get();
    EXPECT_FALSE(admission::CommandAdmissionIndexTestAccess::admissionOpen(*script.admissionIndex));
    const auto activation =
        storage::detail::activatePreparedNewRunV1WithHooks(catalogPath, prepared, script.activationHooks);
    EXPECT_FALSE(admission::CommandAdmissionIndexTestAccess::admissionOpen(*script.admissionIndex));
    return activation;
}

TEST(ControllerAdmissionGateTest, CommittedStartupOpensAndClosingPreservesCompletionAndRetransmissions) {
    StartupDirectory directory;
    ASSERT_TRUE(directory.valid());
    ExchangeRunController controller;
    NewControllerStartScript script{.activationHooks = storage::detail::systemRunCatalogStorageHooks()};
    const detail::StartNewRunHooks hooks{&script, constructControllerRun, activateControllerRun};
    ASSERT_EQ(
        detail::startupNewRunV1WithHooks(directory.catalog(), controllerConfiguration(), &hooks, controller).outcome,
        NewRunStartupOutcome::READY);
    ASSERT_NE(script.admissionIndex, nullptr);
    auto command = sellLookup();
    ASSERT_EQ(script.admissionIndex->reserve(command).status, admission::AdmissionStatus::FIRST_SUBMISSION);
    command.globalSequenceNumber = script.writer->nextCommandSequence();
    command.orderId = domain::OrderId{command.globalSequenceNumber.value()};
    const storage::JournalNewOrderV1 persisted{
        .exchangeRunId = script.writer->header().exchangeRunId,
        .commandSequence = command.globalSequenceNumber,
        .behavioralRulesVersion = 1,
        .configurationVersion = 1,
        .clientId = command.clientId,
        .instrumentId = command.instrumentId,
        .clientCommandId = *command.clientCommandId,
        .side = domain::Side::SELL,
        .timeInForce = command.tif,
        .price = command.price,
        .quantity = command.quantity,
    };
    ASSERT_EQ(script.writer->append(persisted).outcome, storage::RunJournalAppendOutcome::COMMITTED);
    ASSERT_EQ(script.admissionIndex->markSequenced(command), admission::MarkSequencedStatus::SEQUENCED);
    const auto result = script.matchingState->processCommand(command);
    ASSERT_NE(result, nullptr);

    controller.closeAdmission();

    EXPECT_EQ(controller.state(), ExchangeRunStartupState::READY); // No pause/drain transition.
    EXPECT_EQ(script.admissionIndex->reserve(command).status, admission::AdmissionStatus::IDENTICAL_IN_FLIGHT);
    ASSERT_EQ(script.admissionIndex->complete(result), admission::CompletionStatus::COMPLETED);
    const auto duplicate = script.admissionIndex->reserve(command);
    EXPECT_EQ(duplicate.status, admission::AdmissionStatus::IDENTICAL_COMPLETED);
    EXPECT_EQ(duplicate.originalResult, result);
    auto conflict = command;
    conflict.quantity = domain::Quantity{11};
    const auto conflicting = script.admissionIndex->reserve(conflict);
    EXPECT_EQ(conflicting.status, admission::AdmissionStatus::CONFLICTING_REUSE);
    EXPECT_EQ(conflicting.rejectionReason, domain::AdmissionRejectionReason::DUPLICATE_COMMAND_CONFLICT);
    auto fresh = command;
    fresh.clientCommandId = domain::ClientCommandId{"FRESH"};
    EXPECT_EQ(script.admissionIndex->reserve(fresh).status, admission::AdmissionStatus::ADMISSION_UNAVAILABLE);
    EXPECT_EQ(script.admissionIndex->size(), 1U);
    EXPECT_EQ(script.admissionIndex->completedResult(command), result);
    EXPECT_EQ(script.writer->nextCommandSequence(), domain::CommandSequence{2});
}

TEST(ControllerAdmissionGateTest, ConcurrentCloseLinearizesWithCompleteAdmissionAndStaging) {
    StartupDirectory directory;
    ASSERT_TRUE(directory.valid());
    ExchangeRunController controller;
    NewControllerStartScript script{.activationHooks = storage::detail::systemRunCatalogStorageHooks()};
    const detail::StartNewRunHooks hooks{&script, constructControllerRun, activateControllerRun};
    ASSERT_EQ(
        detail::startupNewRunV1WithHooks(directory.catalog(), controllerConfiguration(), &hooks, controller).outcome,
        NewRunStartupOutcome::READY);
    constexpr std::size_t PRODUCERS = 8;
    SharedQueue<sequencer::sequenceMessage> staging(PRODUCERS);
    std::barrier start(static_cast<std::ptrdiff_t>(PRODUCERS + 1));
    std::barrier closed(static_cast<std::ptrdiff_t>(PRODUCERS + 1));
    std::array<sequencer::sequenceMessage, PRODUCERS> commands{};
    std::array<admission::AdmissionDecision, PRODUCERS> racing{};
    std::array<admission::AdmissionDecision, PRODUCERS> afterClose{};
    std::vector<std::thread> producers;
    for (std::size_t i = 0; i < PRODUCERS; ++i) {
        commands[i] = sellLookup();
        commands[i].clientCommandId = domain::ClientCommandId{"RACE-" + std::to_string(i)};
        producers.emplace_back([&, i] {
            auto command = commands[i];
            start.arrive_and_wait();
            racing[i] = script.admissionIndex->reserveAndStage(command, staging);
            closed.arrive_and_wait();
            command.clientCommandId = domain::ClientCommandId{"AFTER-" + std::to_string(i)};
            afterClose[i] = script.admissionIndex->reserveAndStage(command, staging);
        });
    }
    start.arrive_and_wait();
    controller.closeAdmission();
    const auto reservedAtClose = script.admissionIndex->size();
    std::array<bool, PRODUCERS> seen{};
    std::size_t stagedAtClose = 0;
    sequencer::sequenceMessage staged{};
    while (staging.pop(staged)) {
        std::size_t i = 0;
        while (i < PRODUCERS && commands[i].clientCommandId != staged.clientCommandId) {
            ++i;
        }
        EXPECT_LT(i, PRODUCERS);
        if (i < PRODUCERS) {
            EXPECT_FALSE(seen[i]);
            seen[i] = true;
        }
        ++stagedAtClose;
    }
    EXPECT_EQ(stagedAtClose, reservedAtClose);
    closed.arrive_and_wait();
    for (auto &producer : producers) {
        producer.join();
    }
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < PRODUCERS; ++i) {
        EXPECT_TRUE(racing[i].status == admission::AdmissionStatus::FIRST_SUBMISSION ||
                    racing[i].status == admission::AdmissionStatus::ADMISSION_UNAVAILABLE);
        accepted += racing[i].status == admission::AdmissionStatus::FIRST_SUBMISSION;
        EXPECT_EQ(seen[i], racing[i].status == admission::AdmissionStatus::FIRST_SUBMISSION);
        EXPECT_EQ(racing[i].rejectionReason,
                  seen[i] ? std::nullopt : std::optional{domain::AdmissionRejectionReason::EXCHANGE_RUN_UNAVAILABLE});
        EXPECT_EQ(afterClose[i].status, admission::AdmissionStatus::ADMISSION_UNAVAILABLE);
        EXPECT_EQ(afterClose[i].rejectionReason, domain::AdmissionRejectionReason::EXCHANGE_RUN_UNAVAILABLE);
        EXPECT_EQ(script.admissionIndex->reserveAndStage(commands[i], staging).status,
                  racing[i].status == admission::AdmissionStatus::FIRST_SUBMISSION
                      ? admission::AdmissionStatus::IDENTICAL_IN_FLIGHT
                      : admission::AdmissionStatus::ADMISSION_UNAVAILABLE);
    }
    EXPECT_EQ(script.admissionIndex->size(), accepted);
    EXPECT_EQ(script.admissionIndex->statistics().firstSubmissions, accepted);
    EXPECT_EQ(accepted, reservedAtClose);
    EXPECT_TRUE(staging.empty());
    EXPECT_EQ(script.writer->committedCommandCount(), 0U);
}

TEST(NewRunControllerStartupTest, ConstructionFailuresNeverActivateOrExposeReadyOwners) {
    for (const auto failure : {StartNewRunOutcome::CAPACITY_UNREPRESENTABLE, StartNewRunOutcome::ALLOCATION_FAILURE,
                               StartNewRunOutcome::STATE_CONSTRUCTION_FAILED}) {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        ExchangeRunController controller;
        NewControllerStartScript script{.constructionFailure = failure};
        const detail::StartNewRunHooks hooks{&script, constructControllerRun, activateControllerRun};

        const auto result =
            detail::startupNewRunV1WithHooks(directory.catalog(), controllerConfiguration(), &hooks, controller);

        EXPECT_EQ(result.outcome, NewRunStartupOutcome::START_FAILED);
        ASSERT_TRUE(result.startup.has_value());
        EXPECT_EQ(result.startup->outcome, failure);
        EXPECT_EQ(result.startup->preparation.outcome, storage::NewRunJournalPreparationOutcome::PREPARED);
        EXPECT_FALSE(result.startup->activation.has_value());
        EXPECT_EQ(script.constructionCalls, 1);
        EXPECT_EQ(script.activationCalls, 0);
        expectStartupUnavailable(controller);
        EXPECT_TRUE(std::filesystem::exists(directory.journal(1)));
        storage::RunCatalogSnapshotV1 catalog;
        ASSERT_EQ(storage::loadRunCatalogV1(directory.catalog(), catalog).outcome,
                  storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(catalog.lastReservedRunId, domain::ExchangeRunId{1});
        EXPECT_FALSE(catalog.activeRunId.has_value());
    }
}

TEST(NewRunControllerStartupTest, ReadyOwnersRequireCommittedActivationAndRemainUnavailableOnRealUncertainty) {
    for (const auto expected :
         {storage::RunCatalogReplaceOutcome::COMMITTED, storage::RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE,
          storage::RunCatalogReplaceOutcome::UNCERTAIN}) {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        ExchangeRunController controller;
        StartupSyncScript sync{&controller, expected == storage::RunCatalogReplaceOutcome::UNCERTAIN};
        auto storageHooks = storage::detail::systemRunCatalogStorageHooks();
        storageHooks.context = &sync;
        storageHooks.syncFile = startupCatalogSync;
        if (expected == storage::RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE) {
            storageHooks.writeFile = failStartupCatalogWrite;
        }
        NewControllerStartScript script{.activationHooks = storageHooks, .sync = &sync};
        const detail::StartNewRunHooks hooks{&script, constructControllerRun, activateControllerRun};

        const auto result =
            detail::startupNewRunV1WithHooks(directory.catalog(), controllerConfiguration(), &hooks, controller);

        ASSERT_TRUE(result.startup.has_value());
        ASSERT_TRUE(result.startup->activation.has_value());
        ASSERT_TRUE(result.startup->activation->catalogReplacement.has_value());
        const auto &replacement = *result.startup->activation->catalogReplacement;
        EXPECT_EQ(replacement.outcome, expected);
        EXPECT_EQ(script.constructionCalls, 1);
        EXPECT_EQ(script.activationCalls, 1);
        EXPECT_TRUE(sync.unavailableUntilSync);
        EXPECT_EQ(sync.calls, expected == storage::RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE ? 0 : 2);
        storage::LoadedRunJournalV1 journal;
        ASSERT_EQ(storage::loadRunJournalV1(directory.journal(1), journal).outcome,
                  storage::RunJournalLoadOutcome::VALID);
        EXPECT_EQ(journal.header, startupHeader(domain::ExchangeRunId{1}));
        EXPECT_TRUE(journal.commands.empty());
        storage::RunCatalogSnapshotV1 catalog;
        ASSERT_EQ(storage::loadRunCatalogV1(directory.catalog(), catalog).outcome,
                  storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(catalog.lastReservedRunId, domain::ExchangeRunId{1});
        if (expected == storage::RunCatalogReplaceOutcome::COMMITTED) {
            EXPECT_EQ(result.outcome, NewRunStartupOutcome::READY);
            EXPECT_EQ(result.startup->outcome, StartNewRunOutcome::STARTED);
            EXPECT_EQ(controller.state(), ExchangeRunStartupState::READY);
            EXPECT_NE(controller.matchingState(), nullptr);
            EXPECT_NE(controller.admissionIndex(), nullptr);
            EXPECT_TRUE(admission::CommandAdmissionIndexTestAccess::admissionOpen(*controller.admissionIndex()));
            EXPECT_EQ(catalog.activeDisposition, storage::RunCatalogDisposition::OPEN);
        } else {
            EXPECT_EQ(result.outcome, NewRunStartupOutcome::START_FAILED);
            EXPECT_EQ(result.startup->outcome, StartNewRunOutcome::ACTIVATION_FAILED);
            EXPECT_EQ(result.startup->activation->outcome, storage::NewRunActivationOutcome::CATALOG_REPLACE_FAILED);
            EXPECT_EQ(replacement.systemError, EIO);
            expectStartupUnavailable(controller);
            if (expected == storage::RunCatalogReplaceOutcome::UNCERTAIN) {
                EXPECT_EQ(catalog.activeRunId, domain::ExchangeRunId{1});
                EXPECT_EQ(catalog.activeDisposition, storage::RunCatalogDisposition::OPEN);
                ASSERT_TRUE(replacement.observedSnapshot.has_value());
                EXPECT_EQ(*replacement.observedSnapshot, catalog);
            } else {
                EXPECT_FALSE(catalog.activeRunId.has_value());
            }
        }
    }
}

struct ResumeSyncScript final {
    const ExchangeRunController *controller;
    bool failDirectorySync{false};
    int calls{0};
};

int resumeCatalogSync(void *context, const int descriptor) noexcept {
    auto &script = *static_cast<ResumeSyncScript *>(context);
    ++script.calls;
    EXPECT_EQ(script.controller->state(), ExchangeRunStartupState::PAUSED);
    EXPECT_NE(script.controller->journalWriter(), nullptr);
    EXPECT_NE(script.controller->matchingState(), nullptr);
    EXPECT_NE(script.controller->admissionIndex(), nullptr);
    EXPECT_FALSE(admission::CommandAdmissionIndexTestAccess::admissionOpen(*script.controller->admissionIndex()));
    if (script.calls == 2 && script.failDirectorySync) {
        errno = EIO;
        return -1;
    }
    return ::fsync(descriptor);
}

TEST(RunResumeTest, DurableOpenPreservesOwnersResultsBooksAndSequenceEvenWhenDirectorySyncIsUncertain) {
    for (const bool uncertain : {false, true}) {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        createStartupJournal(directory.journal(), RUN_ID, true);
        ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog()).outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        ExchangeRunController controller;
        ASSERT_EQ(controller.startupExistingRunV1(directory.catalog()).outcome, ExistingRunStartupOutcome::PAUSED);
        const auto *writer = controller.journalWriter();
        const auto *matching = controller.matchingState();
        const auto *admission = controller.admissionIndex();
        ASSERT_NE(writer, nullptr);
        const auto header = writer->header();
        const auto count = writer->committedCommandCount();
        const auto bytes = writer->committedByteCount();
        const auto next = writer->nextCommandSequence();
        const auto books = matching->snapshot();
        const auto completed = admission->completedResult(sellLookup());
        ASSERT_NE(completed, nullptr);
        const auto journalBytes = startupBytes(directory.journal());
        ResumeSyncScript script{&controller, uncertain};
        auto hooks = storage::detail::systemRunCatalogStorageHooks();
        hooks.context = &script;
        hooks.syncFile = resumeCatalogSync;

        const auto result = detail::resumeRunV1WithHooks(hooks, controller);

        EXPECT_EQ(admission::CommandAdmissionIndexTestAccess::admissionOpen(*admission), !uncertain);
        EXPECT_EQ(script.calls, 2);
        ASSERT_TRUE(result.catalogLoad.has_value());
        EXPECT_EQ(result.catalogLoad->outcome, storage::RunCatalogLoadOutcome::LOADED);
        ASSERT_TRUE(result.catalogReplacement.has_value());
        EXPECT_EQ(controller.journalWriter(), writer);
        EXPECT_EQ(controller.matchingState(), matching);
        EXPECT_EQ(controller.admissionIndex(), admission);
        EXPECT_EQ(writer->header(), header);
        EXPECT_EQ(writer->committedCommandCount(), count);
        EXPECT_EQ(count, 3U);
        EXPECT_EQ(writer->committedByteCount(), bytes);
        EXPECT_EQ(writer->nextCommandSequence(), next);
        EXPECT_EQ(next, domain::CommandSequence{4});
        EXPECT_EQ(matching->snapshot(), books);
        EXPECT_EQ(admission->completedResult(sellLookup()), completed);
        EXPECT_EQ(admission->statistics(), admission::AdmissionStatistics{});
        EXPECT_EQ(startupBytes(directory.journal()), journalBytes);
        auto expected = startupCatalog();
        expected.generation = 3;
        storage::RunCatalogSnapshotV1 observed;
        ASSERT_EQ(storage::loadRunCatalogV1(directory.catalog(), observed).outcome,
                  storage::RunCatalogLoadOutcome::LOADED);
        EXPECT_EQ(observed, expected);
        if (uncertain) {
            EXPECT_EQ(result.outcome, RunResumeOutcome::CATALOG_REPLACE_FAILED);
            EXPECT_EQ(result.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::UNCERTAIN);
            EXPECT_EQ(result.catalogReplacement->systemError, EIO);
            ASSERT_TRUE(result.catalogReplacement->observedSnapshot.has_value());
            EXPECT_EQ(*result.catalogReplacement->observedSnapshot, observed);
            EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
            const auto catalogBytes = startupBytes(directory.catalog());
            const auto refused = controller.resumeRunV1();
            EXPECT_EQ(refused.outcome, RunResumeOutcome::CATALOG_NOT_PAUSED);
            EXPECT_FALSE(refused.catalogReplacement.has_value());
            EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
            EXPECT_EQ(startupBytes(directory.catalog()), catalogBytes);
        } else {
            EXPECT_EQ(result.outcome, RunResumeOutcome::READY);
            EXPECT_EQ(result.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::COMMITTED);
            EXPECT_EQ(controller.state(), ExchangeRunStartupState::READY);
        }
    }
}

TEST(RunResumeTest, UnavailableRecoveryFailedAndReadyControllersRefuseWithoutCatalogMutation) {
    StartupDirectory directory;
    ASSERT_TRUE(directory.valid());
    ExchangeRunController controller;
    const auto unowned = controller.resumeRunV1();
    EXPECT_EQ(unowned.outcome, RunResumeOutcome::NOT_PAUSED);
    EXPECT_FALSE(unowned.catalogLoad.has_value());
    EXPECT_FALSE(unowned.catalogReplacement.has_value());
    expectStartupUnavailable(controller);
    EXPECT_FALSE(std::filesystem::exists(directory.catalog()));
    ASSERT_EQ(controller.startupNewRunV1(directory.catalog(), controllerConfiguration()).outcome,
              NewRunStartupOutcome::READY);
    const auto *writer = controller.journalWriter();
    const auto *matching = controller.matchingState();
    const auto *admission = controller.admissionIndex();
    const auto catalog = startupBytes(directory.catalog());
    const auto journal = startupBytes(directory.journal(1));

    const auto ready = controller.resumeRunV1();

    EXPECT_EQ(ready.outcome, RunResumeOutcome::NOT_PAUSED);
    EXPECT_FALSE(ready.catalogLoad.has_value());
    EXPECT_FALSE(ready.catalogReplacement.has_value());
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::READY);
    EXPECT_EQ(controller.journalWriter(), writer);
    EXPECT_EQ(controller.matchingState(), matching);
    EXPECT_EQ(controller.admissionIndex(), admission);
    EXPECT_EQ(startupBytes(directory.catalog()), catalog);
    EXPECT_EQ(startupBytes(directory.journal(1)), journal);

    StartupDirectory failedDirectory;
    ASSERT_TRUE(failedDirectory.valid());
    ASSERT_EQ(storage::replaceRunCatalogV1(failedDirectory.catalog(), startupCatalog()).outcome,
              storage::RunCatalogReplaceOutcome::COMMITTED);
    ExchangeRunController failedController;
    ASSERT_EQ(failedController.startupExistingRunV1(failedDirectory.catalog()).outcome,
              ExistingRunStartupOutcome::RECOVERY_FAILED);
    const auto failedCatalog = startupBytes(failedDirectory.catalog());
    const auto failed = failedController.resumeRunV1();
    EXPECT_EQ(failed.outcome, RunResumeOutcome::NOT_PAUSED);
    EXPECT_FALSE(failed.catalogLoad.has_value());
    EXPECT_FALSE(failed.catalogReplacement.has_value());
    expectStartupUnavailable(failedController, ExchangeRunStartupState::RECOVERY_FAILED);
    EXPECT_EQ(startupBytes(failedDirectory.catalog()), failedCatalog);
    EXPECT_FALSE(std::filesystem::exists(failedDirectory.journal()));
}

TEST(RunResumeTest, RevalidatesOriginalCatalogBindingAndRefusesMissingOrDifferentActiveSelection) {
    for (const bool noActive : {false, true}) {
        StartupDirectory directory;
        StartupDirectory otherDirectory;
        ASSERT_TRUE(directory.valid());
        ASSERT_TRUE(otherDirectory.valid());
        createStartupJournal(directory.journal(), RUN_ID, true);
        ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog()).outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        ExchangeRunController controller;
        ASSERT_EQ(controller.startupExistingRunV1(directory.catalog()).outcome, ExistingRunStartupOutcome::PAUSED);
        ASSERT_EQ(storage::replaceRunCatalogV1(otherDirectory.catalog(),
                                               startupCatalog(storage::RunCatalogDisposition::PAUSED))
                      .outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        auto changed = startupCatalog(storage::RunCatalogDisposition::PAUSED);
        changed.generation = 3;
        if (noActive) {
            changed.activeRunId.reset();
            changed.activeDisposition = storage::RunCatalogDisposition::NONE;
        } else {
            changed.activeRunId = domain::ExchangeRunId{43};
        }
        ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), changed).outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        const auto *writer = controller.journalWriter();
        const auto *matching = controller.matchingState();
        const auto *admission = controller.admissionIndex();
        const auto completed = admission->completedResult(sellLookup());
        const auto catalog = startupBytes(directory.catalog());
        const auto otherCatalog = startupBytes(otherDirectory.catalog());
        const auto journal = startupBytes(directory.journal());

        const auto result = controller.resumeRunV1();

        EXPECT_EQ(result.outcome, RunResumeOutcome::ACTIVE_RUN_ID_MISMATCH);
        EXPECT_FALSE(result.catalogReplacement.has_value());
        EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
        EXPECT_EQ(controller.journalWriter(), writer);
        EXPECT_EQ(controller.matchingState(), matching);
        EXPECT_EQ(controller.admissionIndex(), admission);
        EXPECT_EQ(admission->completedResult(sellLookup()), completed);
        EXPECT_EQ(startupBytes(directory.catalog()), catalog);
        EXPECT_EQ(startupBytes(otherDirectory.catalog()), otherCatalog);
        EXPECT_EQ(startupBytes(directory.journal()), journal);
    }
}

TEST(RunResumeTest, OtherCatalogDispositionsNeverBecomeOpenIncludingCapacityReached) {
    for (const auto disposition :
         {storage::RunCatalogDisposition::OPEN, storage::RunCatalogDisposition::CAPACITY_REACHED,
          storage::RunCatalogDisposition::FAIL_STOPPED, storage::RunCatalogDisposition::RECOVERY_FAILED}) {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        createStartupJournal(directory.journal());
        ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog()).outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        ExchangeRunController controller;
        ASSERT_EQ(controller.startupExistingRunV1(directory.catalog()).outcome, ExistingRunStartupOutcome::PAUSED);
        auto changed = startupCatalog(disposition);
        changed.generation = 3;
        ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), changed).outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        const auto *writer = controller.journalWriter();
        const auto catalog = startupBytes(directory.catalog());
        const auto journal = startupBytes(directory.journal());

        const auto result = controller.resumeRunV1();

        EXPECT_EQ(result.outcome, RunResumeOutcome::CATALOG_NOT_PAUSED);
        EXPECT_FALSE(result.catalogReplacement.has_value());
        EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
        EXPECT_EQ(controller.journalWriter(), writer);
        EXPECT_EQ(startupBytes(directory.catalog()), catalog);
        EXPECT_EQ(startupBytes(directory.journal()), journal);
    }
}

TEST(RunResumeTest, MissingInvalidAndUnreadableCatalogsRetainOwnersAndExactLoadErrors) {
    for (const auto outcome : {storage::RunCatalogLoadOutcome::MISSING, storage::RunCatalogLoadOutcome::INVALID,
                               storage::RunCatalogLoadOutcome::IO_FAILURE}) {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        createStartupJournal(directory.journal(), RUN_ID, true);
        ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog()).outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        ExchangeRunController controller;
        ASSERT_EQ(controller.startupExistingRunV1(directory.catalog()).outcome, ExistingRunStartupOutcome::PAUSED);
        const auto *writer = controller.journalWriter();
        const auto *matching = controller.matchingState();
        const auto *admission = controller.admissionIndex();
        const auto completed = admission->completedResult(sellLookup());
        const auto journal = startupBytes(directory.journal());
        std::filesystem::rename(directory.catalog(), directory.catalog().string() + ".saved");
        if (outcome == storage::RunCatalogLoadOutcome::INVALID) {
            const std::array<char, 1> invalid{'X'};
            writeStartupBytes(directory.catalog(), invalid);
        } else if (outcome == storage::RunCatalogLoadOutcome::IO_FAILURE) {
            ASSERT_TRUE(std::filesystem::create_directory(directory.catalog()));
        }
        storage::RunCatalogSnapshotV1 ignored;
        const auto expected = storage::loadRunCatalogV1(directory.catalog(), ignored);

        const auto result = controller.resumeRunV1();

        EXPECT_EQ(result.outcome, RunResumeOutcome::CATALOG_LOAD_FAILED);
        ASSERT_TRUE(result.catalogLoad.has_value());
        EXPECT_EQ(result.catalogLoad->outcome, outcome);
        EXPECT_EQ(result.catalogLoad->codecError, expected.codecError);
        EXPECT_EQ(result.catalogLoad->systemError, expected.systemError);
        EXPECT_FALSE(result.catalogReplacement.has_value());
        EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
        EXPECT_EQ(controller.journalWriter(), writer);
        EXPECT_EQ(controller.matchingState(), matching);
        EXPECT_EQ(controller.admissionIndex(), admission);
        EXPECT_EQ(admission->completedResult(sellLookup()), completed);
        EXPECT_EQ(startupBytes(directory.journal()), journal);
    }
}

TEST(RunResumeTest, IndependentlyRefusesExhaustedCommandAndByteCapacityWithoutWriting) {
    for (const bool bytesFull : {false, true}) {
        StartupDirectory directory;
        ASSERT_TRUE(directory.valid());
        auto header = startupHeader();
        if (bytesFull) {
            std::vector<std::byte> encoded;
            ASSERT_EQ(storage::encodeRunHeaderV1(header, encoded), storage::RunHeaderCodecError::NONE);
            header.maxRunJournalBytes = encoded.size();
        } else {
            header.maxRunCommands = 1;
        }
        std::unique_ptr<storage::RunJournalWriterV1> originalWriter;
        ASSERT_EQ(storage::createRunJournalWriterV1(directory.journal(), header, originalWriter).outcome,
                  storage::RunJournalCreateOutcome::CREATED);
        if (!bytesFull) {
            const storage::JournalCancelV1 cancel{
                .exchangeRunId = RUN_ID,
                .commandSequence = domain::CommandSequence{1},
                .behavioralRulesVersion = 1,
                .configurationVersion = 1,
                .clientId = domain::ClientId{11},
                .instrumentId = domain::InstrumentId{1},
                .clientCommandId = domain::ClientCommandId{"CANCEL"},
                .targetOrderId = domain::TargetOrderId{999},
            };
            ASSERT_EQ(originalWriter->append(cancel).outcome, storage::RunJournalAppendOutcome::COMMITTED);
        }
        originalWriter.reset();
        ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog()).outcome,
                  storage::RunCatalogReplaceOutcome::COMMITTED);
        ExchangeRunController controller;
        ASSERT_EQ(controller.startupExistingRunV1(directory.catalog()).outcome, ExistingRunStartupOutcome::PAUSED);
        const auto *writer = controller.journalWriter();
        const auto catalog = startupBytes(directory.catalog());
        const auto journal = startupBytes(directory.journal());

        const auto result = controller.resumeRunV1();

        EXPECT_EQ(result.outcome, RunResumeOutcome::CAPACITY_REACHED);
        EXPECT_FALSE(result.catalogReplacement.has_value());
        EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
        EXPECT_EQ(controller.journalWriter(), writer);
        EXPECT_EQ(writer->header(), header);
        EXPECT_EQ(writer->committedCommandCount(), bytesFull ? 0U : 1U);
        EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{bytesFull ? 1U : 2U});
        EXPECT_EQ(startupBytes(directory.catalog()), catalog);
        EXPECT_EQ(startupBytes(directory.journal()), journal);
    }
}

TEST(RunResumeTest, GenerationExhaustionDoesNotWrapOrMutateOwnedState) {
    StartupDirectory directory;
    ASSERT_TRUE(directory.valid());
    createStartupJournal(directory.journal());
    ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog()).outcome,
              storage::RunCatalogReplaceOutcome::COMMITTED);
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupExistingRunV1(directory.catalog()).outcome, ExistingRunStartupOutcome::PAUSED);
    auto exhausted = startupCatalog(storage::RunCatalogDisposition::PAUSED);
    exhausted.generation = std::numeric_limits<std::uint64_t>::max();
    storage::RunCatalogV1Bytes encoded;
    ASSERT_EQ(storage::encodeRunCatalogV1(exhausted, encoded), storage::RunCatalogCodecError::NONE);
    writeStartupBytes(directory.catalog(), {reinterpret_cast<const char *>(encoded.data()), encoded.size()});
    const auto *writer = controller.journalWriter();
    const auto catalog = startupBytes(directory.catalog());
    const auto journal = startupBytes(directory.journal());

    const auto result = controller.resumeRunV1();

    EXPECT_EQ(result.outcome, RunResumeOutcome::GENERATION_EXHAUSTED);
    EXPECT_FALSE(result.catalogReplacement.has_value());
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
    EXPECT_EQ(controller.journalWriter(), writer);
    EXPECT_EQ(startupBytes(directory.catalog()), catalog);
    EXPECT_EQ(startupBytes(directory.journal()), journal);
}

TEST(RunResumeTest, DefiniteReplacementFailureRetainsAllOwnersResultsAndPausedCatalog) {
    StartupDirectory directory;
    ASSERT_TRUE(directory.valid());
    createStartupJournal(directory.journal(), RUN_ID, true);
    ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog()).outcome,
              storage::RunCatalogReplaceOutcome::COMMITTED);
    ExchangeRunController controller;
    ASSERT_EQ(controller.startupExistingRunV1(directory.catalog()).outcome, ExistingRunStartupOutcome::PAUSED);
    const auto *writer = controller.journalWriter();
    const auto *matching = controller.matchingState();
    const auto *admission = controller.admissionIndex();
    const auto completed = admission->completedResult(sellLookup());
    const auto books = matching->snapshot();
    const auto catalog = startupBytes(directory.catalog());
    const auto journal = startupBytes(directory.journal());
    auto hooks = storage::detail::systemRunCatalogStorageHooks();
    hooks.writeFile = failStartupCatalogWrite;

    const auto result = detail::resumeRunV1WithHooks(hooks, controller);

    EXPECT_EQ(result.outcome, RunResumeOutcome::CATALOG_REPLACE_FAILED);
    ASSERT_TRUE(result.catalogReplacement.has_value());
    EXPECT_EQ(result.catalogReplacement->outcome, storage::RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE);
    EXPECT_EQ(result.catalogReplacement->systemError, EIO);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::PAUSED);
    EXPECT_FALSE(admission::CommandAdmissionIndexTestAccess::admissionOpen(*admission));
    EXPECT_EQ(controller.journalWriter(), writer);
    EXPECT_EQ(controller.matchingState(), matching);
    EXPECT_EQ(controller.admissionIndex(), admission);
    EXPECT_EQ(admission->completedResult(sellLookup()), completed);
    EXPECT_EQ(matching->snapshot(), books);
    EXPECT_EQ(writer->committedCommandCount(), 3U);
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{4});
    EXPECT_EQ(startupBytes(directory.catalog()), catalog);
    EXPECT_EQ(startupBytes(directory.journal()), journal);
}

class CatalogWorkingDirectoryTest : public ::testing::Test {
protected:
    void SetUp() override {
        originalDirectory_ = std::filesystem::current_path();
    }

    void TearDown() override {
        std::error_code error;
        std::filesystem::current_path(originalDirectory_, error);
        EXPECT_FALSE(error);
    }

private:
    std::filesystem::path originalDirectory_;
};

TEST_F(CatalogWorkingDirectoryTest, ExistingStartupAndResumeStayBoundToOriginalInstallation) {
    StartupDirectory directory;
    StartupDirectory otherDirectory;
    ASSERT_TRUE(directory.valid());
    ASSERT_TRUE(otherDirectory.valid());
    createStartupJournal(directory.journal(), RUN_ID, true);
    createStartupJournal(otherDirectory.journal(), RUN_ID, true);
    ASSERT_EQ(storage::replaceRunCatalogV1(directory.catalog(), startupCatalog()).outcome,
              storage::RunCatalogReplaceOutcome::COMMITTED);
    ASSERT_EQ(
        storage::replaceRunCatalogV1(otherDirectory.catalog(), startupCatalog(storage::RunCatalogDisposition::PAUSED))
            .outcome,
        storage::RunCatalogReplaceOutcome::COMMITTED);
    const auto otherCatalog = startupBytes(otherDirectory.catalog());
    const auto originalJournal = startupBytes(directory.journal());
    const auto otherJournal = startupBytes(otherDirectory.journal());
    ExchangeRunController controller;
    std::filesystem::current_path(directory.catalog().parent_path());
    ASSERT_EQ(controller.startupExistingRunV1(directory.catalog().filename()).outcome,
              ExistingRunStartupOutcome::PAUSED);
    const auto *writer = controller.journalWriter();
    const auto *matching = controller.matchingState();
    const auto *admission = controller.admissionIndex();
    const auto completed = admission->completedResult(sellLookup());
    const auto books = matching->snapshot();
    const auto committedBytes = writer->committedByteCount();
    ASSERT_NE(completed, nullptr);
    std::filesystem::current_path(otherDirectory.catalog().parent_path());

    const auto resumed = controller.resumeRunV1();

    EXPECT_EQ(resumed.outcome, RunResumeOutcome::READY);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::READY);
    EXPECT_EQ(controller.journalWriter(), writer);
    EXPECT_EQ(controller.matchingState(), matching);
    EXPECT_EQ(controller.admissionIndex(), admission);
    EXPECT_EQ(admission->completedResult(sellLookup()), completed);
    EXPECT_EQ(matching->snapshot(), books);
    EXPECT_EQ(writer->committedCommandCount(), 3U);
    EXPECT_EQ(writer->committedByteCount(), committedBytes);
    EXPECT_EQ(writer->nextCommandSequence(), domain::CommandSequence{4});
    auto expected = startupCatalog();
    expected.generation = 3;
    storage::RunCatalogSnapshotV1 actual;
    ASSERT_EQ(storage::loadRunCatalogV1(directory.catalog(), actual).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(actual, expected);
    EXPECT_EQ(startupBytes(otherDirectory.catalog()), otherCatalog);
    EXPECT_EQ(startupBytes(directory.journal()), originalJournal);
    EXPECT_EQ(startupBytes(otherDirectory.journal()), otherJournal);
}

TEST_F(CatalogWorkingDirectoryTest, NewStartupUsesAbsoluteBindingThroughPreparationAndActivation) {
    StartupDirectory directory;
    StartupDirectory otherDirectory;
    ASSERT_TRUE(directory.valid());
    ASSERT_TRUE(otherDirectory.valid());
    auto otherParent = otherDirectory.catalog().parent_path();
    const detail::StartNewRunHooks hooks{
        .context = &otherParent,
        .constructState =
            [](void *context, const domain::ExchangeRunId runId, const std::uint64_t capacity,
               std::unique_ptr<matching_engine::MatchingState> &matching,
               std::unique_ptr<admission::CommandAdmissionIndex> &admission) noexcept {
                std::error_code error;
                std::filesystem::current_path(*static_cast<const std::filesystem::path *>(context), error);
                if (error) {
                    return std::optional{StartNewRunOutcome::STATE_CONSTRUCTION_FAILED};
                }
                return detail::constructEmptyRunStateV1(runId, capacity, matching, admission);
            },
        .activate =
            [](void *, const std::filesystem::path &catalogPath,
               const storage::PreparedNewRunJournalV1 &prepared) noexcept {
                EXPECT_TRUE(catalogPath.is_absolute());
                EXPECT_TRUE(prepared.canonicalJournalPath.is_absolute());
                return storage::activatePreparedNewRunV1(catalogPath, prepared);
            },
    };
    ExchangeRunController controller;
    std::filesystem::current_path(directory.catalog().parent_path());

    const auto started =
        detail::startupNewRunV1WithHooks(directory.catalog().filename(), controllerConfiguration(), &hooks, controller);

    EXPECT_EQ(started.outcome, NewRunStartupOutcome::READY);
    EXPECT_EQ(controller.state(), ExchangeRunStartupState::READY);
    ASSERT_NE(controller.journalWriter(), nullptr);
    EXPECT_EQ(controller.journalWriter()->header(), startupHeader(domain::ExchangeRunId{1}));
    EXPECT_EQ(controller.journalWriter()->nextCommandSequence(), domain::CommandSequence{1});
    storage::RunCatalogSnapshotV1 actual;
    ASSERT_EQ(storage::loadRunCatalogV1(directory.catalog(), actual).outcome, storage::RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(actual.generation, 2U);
    EXPECT_EQ(actual.activeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(actual.activeDisposition, storage::RunCatalogDisposition::OPEN);
    storage::LoadedRunJournalV1 journal;
    ASSERT_EQ(storage::loadRunJournalV1(directory.journal(1), journal).outcome, storage::RunJournalLoadOutcome::VALID);
    EXPECT_EQ(journal.header, controller.journalWriter()->header());
    EXPECT_FALSE(std::filesystem::exists(otherDirectory.catalog()));
    EXPECT_FALSE(std::filesystem::exists(otherDirectory.journal(1)));
}

} // namespace
} // namespace exchange::core
