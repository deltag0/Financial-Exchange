#include "new_run_activation.hpp"

#include "new_run_activation_internal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace exchange::storage {
namespace {

class NewRunActivationTestDirectory final {
public:
    NewRunActivationTestDirectory() {
        std::array<char, 52> pathTemplate{};
        constexpr char TEMPLATE[] = "/tmp/exchange-new-run-activation-test-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), pathTemplate.begin());
        char *created = ::mkdtemp(pathTemplate.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~NewRunActivationTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    NewRunActivationTestDirectory(const NewRunActivationTestDirectory &) = delete;
    NewRunActivationTestDirectory &operator=(const NewRunActivationTestDirectory &) = delete;

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

NewRunConfigurationV1 activationConfiguration() {
    return {
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = 10,
        .maxRunJournalBytes = 64 * 1024,
        .instruments = {{domain::InstrumentId{100}, 7}},
    };
}

RunHeaderV1 activationHeader(const domain::ExchangeRunId runId) {
    const NewRunConfigurationV1 configuration = activationConfiguration();
    return {
        .exchangeRunId = runId,
        .behavioralRulesVersion = configuration.behavioralRulesVersion,
        .maxEventsPerCommand = configuration.maxEventsPerCommand,
        .maxRunCommands = configuration.maxRunCommands,
        .maxRunJournalBytes = configuration.maxRunJournalBytes,
        .instruments = configuration.instruments,
    };
}

std::vector<std::byte> activationReadBytes(const std::filesystem::path &path) {
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

void activationWriteBytes(const std::filesystem::path &path, const std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(stream.good());
}

RunCatalogV1Bytes activationCatalogBytes(const RunCatalogSnapshotV1 &snapshot) {
    RunCatalogV1Bytes bytes{};
    EXPECT_EQ(encodeRunCatalogV1(snapshot, bytes), RunCatalogCodecError::NONE);
    return bytes;
}

struct ActivationPreparedSnapshot final {
    RunHeaderV1 header{};
    std::filesystem::path path{};
    RunJournalWriterV1 *writer{nullptr};
    std::uint64_t committedCommands{0};
    std::uint64_t committedBytes{0};
    domain::CommandSequence nextSequence{};
};

ActivationPreparedSnapshot activationSnapshot(const PreparedNewRunJournalV1 &prepared) {
    EXPECT_NE(prepared.writer, nullptr);
    if (prepared.writer == nullptr) {
        return {};
    }
    return {
        .header = prepared.header,
        .path = prepared.canonicalJournalPath,
        .writer = prepared.writer.get(),
        .committedCommands = prepared.writer->committedCommandCount(),
        .committedBytes = prepared.writer->committedByteCount(),
        .nextSequence = prepared.writer->nextCommandSequence(),
    };
}

void expectActivationPreparedUnchanged(const PreparedNewRunJournalV1 &prepared,
                                       const ActivationPreparedSnapshot &snapshot) {
    ASSERT_NE(prepared.writer, nullptr);
    EXPECT_EQ(prepared.header, snapshot.header);
    EXPECT_EQ(prepared.canonicalJournalPath, snapshot.path);
    EXPECT_EQ(prepared.writer.get(), snapshot.writer);
    EXPECT_EQ(prepared.writer->committedCommandCount(), snapshot.committedCommands);
    EXPECT_EQ(prepared.writer->committedByteCount(), snapshot.committedBytes);
    EXPECT_EQ(prepared.writer->nextCommandSequence(), snapshot.nextSequence);
}

void prepareForActivation(NewRunActivationTestDirectory &directory, PreparedNewRunJournalV1 &prepared) {
    ASSERT_TRUE(directory.valid());
    ASSERT_EQ(prepareNewRunJournalV1(directory.catalogPath(), activationConfiguration(), prepared).outcome,
              NewRunJournalPreparationOutcome::PREPARED);
    ASSERT_NE(prepared.writer, nullptr);
}

ssize_t failCatalogWrite(void *, int, const void *, std::size_t) noexcept {
    errno = EIO;
    return -1;
}

struct ActivationSyncScript final {
    int calls{0};
};

int failDirectorySync(void *context, const int descriptor) noexcept {
    auto &script = *static_cast<ActivationSyncScript *>(context);
    ++script.calls;
    if (script.calls == 2) {
        errno = EIO;
        return -1;
    }
    return ::fsync(descriptor);
}

TEST(NewRunActivationTest, ActivatesPreparedRunWithExactCatalogBytesAndPreservesPreparedWriter) {
    NewRunActivationTestDirectory directory;
    PreparedNewRunJournalV1 prepared;
    prepareForActivation(directory, prepared);
    const ActivationPreparedSnapshot original = activationSnapshot(prepared);

    const NewRunActivationResult result = activatePreparedNewRunV1(directory.catalogPath(), prepared);

    EXPECT_EQ(result.outcome, NewRunActivationOutcome::ACTIVATED);
    ASSERT_TRUE(result.catalogLoad.has_value());
    EXPECT_EQ(result.catalogLoad->outcome, RunCatalogLoadOutcome::LOADED);
    ASSERT_TRUE(result.catalogReplacement.has_value());
    EXPECT_EQ(result.catalogReplacement->outcome, RunCatalogReplaceOutcome::COMMITTED);
    const RunCatalogSnapshotV1 expected{
        .generation = 2,
        .lastReservedRunId = domain::ExchangeRunId{1},
        .activeRunId = domain::ExchangeRunId{1},
        .activeDisposition = RunCatalogDisposition::OPEN,
        .retainedStoppedRunId = std::nullopt,
    };
    RunCatalogSnapshotV1 loaded{};
    ASSERT_EQ(loadRunCatalogV1(directory.catalogPath(), loaded).outcome, RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(loaded, expected);
    const RunCatalogV1Bytes expectedBytes = activationCatalogBytes(expected);
    EXPECT_EQ(activationReadBytes(directory.catalogPath()),
              (std::vector<std::byte>{expectedBytes.begin(), expectedBytes.end()}));
    expectActivationPreparedUnchanged(prepared, original);
}

TEST(NewRunActivationTest, PreservesRetainedStoppedRunDuringActivation) {
    NewRunActivationTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const RunCatalogSnapshotV1 retained{
        .generation = 1,
        .lastReservedRunId = domain::ExchangeRunId{1},
        .activeRunId = std::nullopt,
        .activeDisposition = RunCatalogDisposition::NONE,
        .retainedStoppedRunId = domain::ExchangeRunId{1},
    };
    ASSERT_EQ(replaceRunCatalogV1(directory.catalogPath(), retained).outcome, RunCatalogReplaceOutcome::COMMITTED);
    PreparedNewRunJournalV1 prepared;
    prepareForActivation(directory, prepared);
    ASSERT_EQ(prepared.header.exchangeRunId, domain::ExchangeRunId{2});
    const ActivationPreparedSnapshot original = activationSnapshot(prepared);

    const NewRunActivationResult result = activatePreparedNewRunV1(directory.catalogPath(), prepared);

    EXPECT_EQ(result.outcome, NewRunActivationOutcome::ACTIVATED);
    RunCatalogSnapshotV1 loaded{};
    ASSERT_EQ(loadRunCatalogV1(directory.catalogPath(), loaded).outcome, RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(loaded.generation, 3U);
    EXPECT_EQ(loaded.lastReservedRunId, domain::ExchangeRunId{2});
    EXPECT_EQ(loaded.activeRunId, domain::ExchangeRunId{2});
    EXPECT_EQ(loaded.activeDisposition, RunCatalogDisposition::OPEN);
    EXPECT_EQ(loaded.retainedStoppedRunId, domain::ExchangeRunId{1});
    expectActivationPreparedUnchanged(prepared, original);
}

TEST(NewRunActivationTest, RejectsMalformedPreparedInputBeforeCatalogAccessOrMutation) {
    NewRunActivationTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    PreparedNewRunJournalV1 missingWriter{
        .header = activationHeader(domain::ExchangeRunId{1}),
        .canonicalJournalPath = directory.journalPath(1),
    };

    const NewRunActivationResult missingWriterResult = activatePreparedNewRunV1(directory.catalogPath(), missingWriter);

    EXPECT_EQ(missingWriterResult.outcome, NewRunActivationOutcome::MISSING_WRITER);
    EXPECT_FALSE(missingWriterResult.catalogLoad.has_value());
    EXPECT_FALSE(std::filesystem::exists(directory.catalogPath()));

    PreparedNewRunJournalV1 prepared;
    prepareForActivation(directory, prepared);
    const std::vector<std::byte> catalogBefore = activationReadBytes(directory.catalogPath());
    prepared.header.exchangeRunId = domain::ExchangeRunId{};
    const ActivationPreparedSnapshot zeroIdSnapshot = activationSnapshot(prepared);

    const NewRunActivationResult zeroIdResult = activatePreparedNewRunV1(directory.catalogPath(), prepared);

    EXPECT_EQ(zeroIdResult.outcome, NewRunActivationOutcome::ZERO_EXCHANGE_RUN_ID);
    EXPECT_FALSE(zeroIdResult.catalogLoad.has_value());
    EXPECT_EQ(activationReadBytes(directory.catalogPath()), catalogBefore);
    expectActivationPreparedUnchanged(prepared, zeroIdSnapshot);

    prepared.header.exchangeRunId = domain::ExchangeRunId{1};
    prepared.canonicalJournalPath = directory.journalPath(999);
    const ActivationPreparedSnapshot wrongPathSnapshot = activationSnapshot(prepared);

    const NewRunActivationResult wrongPathResult = activatePreparedNewRunV1(directory.catalogPath(), prepared);

    EXPECT_EQ(wrongPathResult.outcome, NewRunActivationOutcome::NONCANONICAL_JOURNAL_PATH);
    EXPECT_FALSE(wrongPathResult.catalogLoad.has_value());
    EXPECT_EQ(activationReadBytes(directory.catalogPath()), catalogBefore);
    expectActivationPreparedUnchanged(prepared, wrongPathSnapshot);
}

TEST(NewRunActivationTest, MissingAndInvalidCatalogsFailClosed) {
    NewRunActivationTestDirectory missingDirectory;
    PreparedNewRunJournalV1 missingPrepared;
    prepareForActivation(missingDirectory, missingPrepared);
    const ActivationPreparedSnapshot missingSnapshot = activationSnapshot(missingPrepared);
    ASSERT_TRUE(std::filesystem::remove(missingDirectory.catalogPath()));

    const NewRunActivationResult missing = activatePreparedNewRunV1(missingDirectory.catalogPath(), missingPrepared);

    EXPECT_EQ(missing.outcome, NewRunActivationOutcome::CATALOG_LOAD_FAILED);
    ASSERT_TRUE(missing.catalogLoad.has_value());
    EXPECT_EQ(missing.catalogLoad->outcome, RunCatalogLoadOutcome::MISSING);
    EXPECT_FALSE(missing.catalogReplacement.has_value());
    expectActivationPreparedUnchanged(missingPrepared, missingSnapshot);

    NewRunActivationTestDirectory invalidDirectory;
    PreparedNewRunJournalV1 invalidPrepared;
    prepareForActivation(invalidDirectory, invalidPrepared);
    const ActivationPreparedSnapshot invalidSnapshot = activationSnapshot(invalidPrepared);
    constexpr std::array<std::byte, 1> INVALID_CATALOG{std::byte{0xff}};
    activationWriteBytes(invalidDirectory.catalogPath(), INVALID_CATALOG);

    const NewRunActivationResult invalid = activatePreparedNewRunV1(invalidDirectory.catalogPath(), invalidPrepared);

    EXPECT_EQ(invalid.outcome, NewRunActivationOutcome::CATALOG_LOAD_FAILED);
    ASSERT_TRUE(invalid.catalogLoad.has_value());
    EXPECT_EQ(invalid.catalogLoad->outcome, RunCatalogLoadOutcome::INVALID);
    EXPECT_EQ(invalid.catalogLoad->codecError, RunCatalogCodecError::INVALID_SIZE);
    EXPECT_FALSE(invalid.catalogReplacement.has_value());
    EXPECT_EQ(activationReadBytes(invalidDirectory.catalogPath()),
              (std::vector<std::byte>{INVALID_CATALOG.begin(), INVALID_CATALOG.end()}));
    expectActivationPreparedUnchanged(invalidPrepared, invalidSnapshot);
}

TEST(NewRunActivationTest, RejectsStaleReservationWithoutReplacingCatalog) {
    NewRunActivationTestDirectory directory;
    PreparedNewRunJournalV1 prepared;
    prepareForActivation(directory, prepared);
    const ActivationPreparedSnapshot original = activationSnapshot(prepared);
    domain::ExchangeRunId laterReservation{};
    ASSERT_EQ(reserveNextExchangeRunId(directory.catalogPath(), laterReservation).outcome,
              ExchangeRunIdReservationOutcome::RESERVED);
    ASSERT_EQ(laterReservation, domain::ExchangeRunId{2});
    const std::vector<std::byte> catalogBefore = activationReadBytes(directory.catalogPath());

    const NewRunActivationResult result = activatePreparedNewRunV1(directory.catalogPath(), prepared);

    EXPECT_EQ(result.outcome, NewRunActivationOutcome::LAST_RESERVED_RUN_ID_MISMATCH);
    EXPECT_FALSE(result.catalogReplacement.has_value());
    EXPECT_EQ(activationReadBytes(directory.catalogPath()), catalogBefore);
    expectActivationPreparedUnchanged(prepared, original);
}

TEST(NewRunActivationTest, RejectsActivationWhileAnotherRunIsActive) {
    NewRunActivationTestDirectory directory;
    PreparedNewRunJournalV1 first;
    prepareForActivation(directory, first);
    ASSERT_EQ(activatePreparedNewRunV1(directory.catalogPath(), first).outcome, NewRunActivationOutcome::ACTIVATED);
    PreparedNewRunJournalV1 second;
    prepareForActivation(directory, second);
    ASSERT_EQ(second.header.exchangeRunId, domain::ExchangeRunId{2});
    const ActivationPreparedSnapshot original = activationSnapshot(second);
    const std::vector<std::byte> catalogBefore = activationReadBytes(directory.catalogPath());

    const NewRunActivationResult result = activatePreparedNewRunV1(directory.catalogPath(), second);

    EXPECT_EQ(result.outcome, NewRunActivationOutcome::ACTIVE_RUN_EXISTS);
    EXPECT_FALSE(result.catalogReplacement.has_value());
    EXPECT_EQ(activationReadBytes(directory.catalogPath()), catalogBefore);
    expectActivationPreparedUnchanged(second, original);
}

TEST(NewRunActivationTest, RejectsGenerationExhaustionWithoutMutation) {
    NewRunActivationTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const RunHeaderV1 header = activationHeader(domain::ExchangeRunId{1});
    std::unique_ptr<RunJournalWriterV1> writer;
    ASSERT_EQ(createRunJournalWriterV1(directory.journalPath(1), header, writer).outcome,
              RunJournalCreateOutcome::CREATED);
    PreparedNewRunJournalV1 prepared{
        .header = header,
        .canonicalJournalPath = directory.journalPath(1),
        .writer = std::move(writer),
    };
    const RunCatalogSnapshotV1 exhausted{
        .generation = std::numeric_limits<std::uint64_t>::max(),
        .lastReservedRunId = domain::ExchangeRunId{1},
        .activeRunId = std::nullopt,
        .activeDisposition = RunCatalogDisposition::NONE,
        .retainedStoppedRunId = std::nullopt,
    };
    const RunCatalogV1Bytes bytes = activationCatalogBytes(exhausted);
    activationWriteBytes(directory.catalogPath(), bytes);
    const ActivationPreparedSnapshot original = activationSnapshot(prepared);

    const NewRunActivationResult result = activatePreparedNewRunV1(directory.catalogPath(), prepared);

    EXPECT_EQ(result.outcome, NewRunActivationOutcome::GENERATION_EXHAUSTED);
    EXPECT_FALSE(result.catalogReplacement.has_value());
    EXPECT_EQ(activationReadBytes(directory.catalogPath()), (std::vector<std::byte>{bytes.begin(), bytes.end()}));
    expectActivationPreparedUnchanged(prepared, original);
}

TEST(NewRunActivationTest, DefiniteReplacementFailurePreservesCatalogAndPreparedWriter) {
    NewRunActivationTestDirectory directory;
    PreparedNewRunJournalV1 prepared;
    prepareForActivation(directory, prepared);
    const ActivationPreparedSnapshot original = activationSnapshot(prepared);
    const std::vector<std::byte> catalogBefore = activationReadBytes(directory.catalogPath());
    auto hooks = detail::systemRunCatalogStorageHooks();
    hooks.writeFile = failCatalogWrite;

    const NewRunActivationResult result =
        detail::activatePreparedNewRunV1WithHooks(directory.catalogPath(), prepared, hooks);

    EXPECT_EQ(result.outcome, NewRunActivationOutcome::CATALOG_REPLACE_FAILED);
    ASSERT_TRUE(result.catalogReplacement.has_value());
    EXPECT_EQ(result.catalogReplacement->outcome, RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE);
    EXPECT_EQ(result.catalogReplacement->systemError, EIO);
    EXPECT_EQ(activationReadBytes(directory.catalogPath()), catalogBefore);
    expectActivationPreparedUnchanged(prepared, original);
}

TEST(NewRunActivationTest, UncertainReplacementNeverReportsActivationAndPreservesPreparedWriter) {
    NewRunActivationTestDirectory directory;
    PreparedNewRunJournalV1 prepared;
    prepareForActivation(directory, prepared);
    const ActivationPreparedSnapshot original = activationSnapshot(prepared);
    ActivationSyncScript script;
    auto hooks = detail::systemRunCatalogStorageHooks();
    hooks.context = &script;
    hooks.syncFile = failDirectorySync;

    const NewRunActivationResult result =
        detail::activatePreparedNewRunV1WithHooks(directory.catalogPath(), prepared, hooks);

    EXPECT_EQ(result.outcome, NewRunActivationOutcome::CATALOG_REPLACE_FAILED);
    ASSERT_TRUE(result.catalogReplacement.has_value());
    EXPECT_EQ(result.catalogReplacement->outcome, RunCatalogReplaceOutcome::UNCERTAIN);
    EXPECT_EQ(result.catalogReplacement->systemError, EIO);
    ASSERT_TRUE(result.catalogReplacement->observedSnapshot.has_value());
    EXPECT_EQ(result.catalogReplacement->observedSnapshot->activeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(result.catalogReplacement->observedSnapshot->activeDisposition, RunCatalogDisposition::OPEN);
    EXPECT_EQ(script.calls, 2);
    expectActivationPreparedUnchanged(prepared, original);
}

} // namespace
} // namespace exchange::storage
