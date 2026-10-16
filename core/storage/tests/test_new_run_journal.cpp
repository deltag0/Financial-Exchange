#include "new_run_journal.hpp"

#include "new_run_journal_internal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

namespace exchange::storage {
namespace {

class NewRunTestDirectory final {
public:
    NewRunTestDirectory() {
        std::array<char, 45> pathTemplate{};
        constexpr char TEMPLATE[] = "/tmp/exchange-new-run-test-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), pathTemplate.begin());
        char *created = ::mkdtemp(pathTemplate.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~NewRunTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    NewRunTestDirectory(const NewRunTestDirectory &) = delete;
    NewRunTestDirectory &operator=(const NewRunTestDirectory &) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return !path_.empty();
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept {
        return path_;
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

NewRunConfigurationV1 newRunConfiguration() {
    return {
        .behavioralRulesVersion = 1,
        .maxEventsPerCommand = 4'096,
        .maxRunCommands = 10,
        .maxRunJournalBytes = 64 * 1024,
        .instruments = {{domain::InstrumentId{100}, 7}, {domain::InstrumentId{200}, 9}},
    };
}

RunHeaderV1 expectedHeader(const domain::ExchangeRunId runId) {
    const NewRunConfigurationV1 configuration = newRunConfiguration();
    return {
        .exchangeRunId = runId,
        .behavioralRulesVersion = configuration.behavioralRulesVersion,
        .maxEventsPerCommand = configuration.maxEventsPerCommand,
        .maxRunCommands = configuration.maxRunCommands,
        .maxRunJournalBytes = configuration.maxRunJournalBytes,
        .instruments = configuration.instruments,
    };
}

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

JournalNewOrderV1 firstOrder(const domain::ExchangeRunId runId) {
    return {
        .exchangeRunId = runId,
        .commandSequence = domain::CommandSequence{1},
        .behavioralRulesVersion = 1,
        .configurationVersion = 7,
        .clientId = domain::ClientId{11},
        .instrumentId = domain::InstrumentId{100},
        .clientCommandId = domain::ClientCommandId{"prepared-new"},
        .side = domain::Side::BUY,
        .timeInForce = core::task::TimeInForce::GTC,
        .price = domain::Price{1'234},
        .quantity = domain::Quantity{50},
    };
}

struct PreparedSnapshot final {
    RunHeaderV1 header{};
    std::filesystem::path path{};
    RunJournalWriterV1 *writer{nullptr};
};

PreparedSnapshot snapshot(const PreparedNewRunJournalV1 &prepared) {
    return {prepared.header, prepared.canonicalJournalPath, prepared.writer.get()};
}

void expectUnchanged(const PreparedNewRunJournalV1 &prepared, const PreparedSnapshot &original) {
    EXPECT_EQ(prepared.header, original.header);
    EXPECT_EQ(prepared.canonicalJournalPath, original.path);
    EXPECT_EQ(prepared.writer.get(), original.writer);
}

void prepareSentinel(NewRunTestDirectory &directory, PreparedNewRunJournalV1 &output) {
    ASSERT_TRUE(directory.valid());
    const auto result = prepareNewRunJournalV1(directory.catalogPath(), newRunConfiguration(), output);
    ASSERT_EQ(result.outcome, NewRunJournalPreparationOutcome::PREPARED);
    ASSERT_NE(output.writer, nullptr);
}

struct CreateScript final {
    RunJournalCreateResult result{};
    bool createEvidence{false};
    int calls{0};
};

RunJournalCreateResult scriptedCreateWriter(void *context, const std::filesystem::path &path, const RunHeaderV1 &,
                                            std::unique_ptr<RunJournalWriterV1> &) noexcept {
    auto &script = *static_cast<CreateScript *>(context);
    ++script.calls;
    if (script.createEvidence) {
        const int descriptor = ::open(path.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, S_IRUSR | S_IWUSR);
        if (descriptor >= 0) {
            constexpr std::array<std::byte, 3> EVIDENCE{
                std::byte{0x01},
                std::byte{0x02},
                std::byte{0x03},
            };
            static_cast<void>(::write(descriptor, EVIDENCE.data(), EVIDENCE.size()));
            static_cast<void>(::close(descriptor));
        }
    }
    return script.result;
}

detail::NewRunJournalPreparationHooks hooksFor(CreateScript &script) {
    return {&script, scriptedCreateWriter};
}

TEST(NewRunJournalTest, FirstPreparationReturnsExactHeaderPathAndAppendReadyWriter) {
    NewRunTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    PreparedNewRunJournalV1 prepared;

    const NewRunJournalPreparationResult result =
        prepareNewRunJournalV1(directory.catalogPath(), newRunConfiguration(), prepared);

    EXPECT_EQ(result.outcome, NewRunJournalPreparationOutcome::PREPARED);
    EXPECT_EQ(result.headerValidation, RunHeaderCodecError::NONE);
    ASSERT_TRUE(result.reservation.has_value());
    EXPECT_EQ(result.reservation->outcome, ExchangeRunIdReservationOutcome::RESERVED);
    ASSERT_TRUE(result.journalCreation.has_value());
    EXPECT_EQ(result.journalCreation->outcome, RunJournalCreateOutcome::CREATED);
    EXPECT_EQ(prepared.header.exchangeRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(prepared.header, expectedHeader(domain::ExchangeRunId{1}));
    EXPECT_EQ(prepared.canonicalJournalPath, directory.journalPath(1));
    ASSERT_NE(prepared.writer, nullptr);
    EXPECT_EQ(prepared.writer->committedCommandCount(), 0U);
    EXPECT_EQ(prepared.writer->nextCommandSequence(), domain::CommandSequence{1});

    std::vector<std::byte> encodedHeader;
    ASSERT_EQ(encodeRunHeaderV1(prepared.header, encodedHeader), RunHeaderCodecError::NONE);
    EXPECT_EQ(readBytes(prepared.canonicalJournalPath), encodedHeader);
    EXPECT_EQ(prepared.writer->committedByteCount(), encodedHeader.size());
    EXPECT_EQ(prepared.writer->append(firstOrder(prepared.header.exchangeRunId)).outcome,
              RunJournalAppendOutcome::COMMITTED);
}

TEST(NewRunJournalTest, SubsequentPreparationUsesTheNextIdAndCanonicalPath) {
    NewRunTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    PreparedNewRunJournalV1 prepared;

    ASSERT_EQ(prepareNewRunJournalV1(directory.catalogPath(), newRunConfiguration(), prepared).outcome,
              NewRunJournalPreparationOutcome::PREPARED);
    const auto result = prepareNewRunJournalV1(directory.catalogPath(), newRunConfiguration(), prepared);

    EXPECT_EQ(result.outcome, NewRunJournalPreparationOutcome::PREPARED);
    EXPECT_EQ(prepared.header.exchangeRunId, domain::ExchangeRunId{2});
    EXPECT_EQ(prepared.header, expectedHeader(domain::ExchangeRunId{2}));
    EXPECT_EQ(prepared.canonicalJournalPath, directory.journalPath(2));
    ASSERT_NE(prepared.writer, nullptr);
    EXPECT_EQ(prepared.writer->nextCommandSequence(), domain::CommandSequence{1});
}

TEST(NewRunJournalTest, InvalidConfigurationFailsBeforeReservationAndPreservesOutput) {
    NewRunTestDirectory sentinelDirectory;
    PreparedNewRunJournalV1 output;
    prepareSentinel(sentinelDirectory, output);
    const PreparedSnapshot original = snapshot(output);
    NewRunTestDirectory targetDirectory;
    ASSERT_TRUE(targetDirectory.valid());
    auto invalid = newRunConfiguration();
    invalid.maxRunCommands = 0;

    const auto result = prepareNewRunJournalV1(targetDirectory.catalogPath(), invalid, output);

    EXPECT_EQ(result.outcome, NewRunJournalPreparationOutcome::HEADER_VALIDATION_FAILED);
    EXPECT_EQ(result.headerValidation, RunHeaderCodecError::ZERO_MAX_RUN_COMMANDS);
    EXPECT_FALSE(result.reservation.has_value());
    EXPECT_FALSE(result.journalCreation.has_value());
    EXPECT_FALSE(std::filesystem::exists(targetDirectory.catalogPath()));
    expectUnchanged(output, original);
}

TEST(NewRunJournalTest, ReservationFailureSkipsJournalCreationAndPreservesOutput) {
    NewRunTestDirectory sentinelDirectory;
    PreparedNewRunJournalV1 output;
    prepareSentinel(sentinelDirectory, output);
    const PreparedSnapshot original = snapshot(output);
    NewRunTestDirectory targetDirectory;
    ASSERT_TRUE(targetDirectory.valid());
    std::ofstream journalEvidence(targetDirectory.journalPath(99), std::ios::binary);
    ASSERT_TRUE(journalEvidence.is_open());
    journalEvidence.close();

    const auto result = prepareNewRunJournalV1(targetDirectory.catalogPath(), newRunConfiguration(), output);

    EXPECT_EQ(result.outcome, NewRunJournalPreparationOutcome::RESERVATION_FAILED);
    ASSERT_TRUE(result.reservation.has_value());
    EXPECT_EQ(result.reservation->outcome, ExchangeRunIdReservationOutcome::JOURNAL_PRESENT_WITHOUT_CATALOG);
    EXPECT_FALSE(result.journalCreation.has_value());
    expectUnchanged(output, original);
}

TEST(NewRunJournalTest, ExistingJournalPathConsumesTheReservedIdAndPreservesEvidence) {
    NewRunTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    PreparedNewRunJournalV1 output;
    ASSERT_EQ(prepareNewRunJournalV1(directory.catalogPath(), newRunConfiguration(), output).outcome,
              NewRunJournalPreparationOutcome::PREPARED);
    const PreparedSnapshot original = snapshot(output);
    constexpr std::array<std::byte, 2> EXISTING_BYTES{std::byte{0x55}, std::byte{0xaa}};
    std::ofstream existing(directory.journalPath(2), std::ios::binary);
    ASSERT_TRUE(existing.is_open());
    existing.write(reinterpret_cast<const char *>(EXISTING_BYTES.data()), EXISTING_BYTES.size());
    existing.close();

    const auto failed = prepareNewRunJournalV1(directory.catalogPath(), newRunConfiguration(), output);

    EXPECT_EQ(failed.outcome, NewRunJournalPreparationOutcome::JOURNAL_CREATION_FAILED);
    ASSERT_TRUE(failed.reservation.has_value());
    EXPECT_EQ(failed.reservation->outcome, ExchangeRunIdReservationOutcome::RESERVED);
    ASSERT_TRUE(failed.journalCreation.has_value());
    EXPECT_EQ(failed.journalCreation->outcome, RunJournalCreateOutcome::PATH_ALREADY_EXISTS);
    EXPECT_EQ(readBytes(directory.journalPath(2)),
              (std::vector<std::byte>{EXISTING_BYTES.begin(), EXISTING_BYTES.end()}));
    expectUnchanged(output, original);

    PreparedNewRunJournalV1 next;
    ASSERT_EQ(prepareNewRunJournalV1(directory.catalogPath(), newRunConfiguration(), next).outcome,
              NewRunJournalPreparationOutcome::PREPARED);
    EXPECT_EQ(next.header.exchangeRunId, domain::ExchangeRunId{3});
    EXPECT_EQ(next.canonicalJournalPath, directory.journalPath(3));
}

TEST(NewRunJournalTest, DefiniteCreationFailureConsumesIdWithoutChangingOutput) {
    NewRunTestDirectory sentinelDirectory;
    PreparedNewRunJournalV1 output;
    prepareSentinel(sentinelDirectory, output);
    const PreparedSnapshot original = snapshot(output);
    NewRunTestDirectory targetDirectory;
    ASSERT_TRUE(targetDirectory.valid());
    CreateScript script{
        .result = {RunJournalCreateOutcome::IO_FAILURE, RunHeaderCodecError::NONE, EIO},
    };
    const auto hooks = hooksFor(script);

    const auto failed =
        detail::prepareNewRunJournalV1WithHooks(targetDirectory.catalogPath(), newRunConfiguration(), hooks, output);

    EXPECT_EQ(failed.outcome, NewRunJournalPreparationOutcome::JOURNAL_CREATION_FAILED);
    ASSERT_TRUE(failed.journalCreation.has_value());
    EXPECT_EQ(failed.journalCreation->outcome, RunJournalCreateOutcome::IO_FAILURE);
    EXPECT_EQ(failed.journalCreation->systemError, EIO);
    EXPECT_EQ(script.calls, 1);
    expectUnchanged(output, original);

    PreparedNewRunJournalV1 next;
    ASSERT_EQ(prepareNewRunJournalV1(targetDirectory.catalogPath(), newRunConfiguration(), next).outcome,
              NewRunJournalPreparationOutcome::PREPARED);
    EXPECT_EQ(next.header.exchangeRunId, domain::ExchangeRunId{2});
}

TEST(NewRunJournalTest, UncertainCreationFailurePreservesEvidenceAndConsumesId) {
    NewRunTestDirectory sentinelDirectory;
    PreparedNewRunJournalV1 output;
    prepareSentinel(sentinelDirectory, output);
    const PreparedSnapshot original = snapshot(output);
    NewRunTestDirectory targetDirectory;
    ASSERT_TRUE(targetDirectory.valid());
    CreateScript script{
        .result = {RunJournalCreateOutcome::UNCERTAIN, RunHeaderCodecError::NONE, EIO},
        .createEvidence = true,
    };
    const auto hooks = hooksFor(script);

    const auto failed =
        detail::prepareNewRunJournalV1WithHooks(targetDirectory.catalogPath(), newRunConfiguration(), hooks, output);

    EXPECT_EQ(failed.outcome, NewRunJournalPreparationOutcome::JOURNAL_CREATION_FAILED);
    ASSERT_TRUE(failed.journalCreation.has_value());
    EXPECT_EQ(failed.journalCreation->outcome, RunJournalCreateOutcome::UNCERTAIN);
    EXPECT_EQ(failed.journalCreation->systemError, EIO);
    const auto evidence = readBytes(targetDirectory.journalPath(1));
    EXPECT_EQ(evidence, (std::vector<std::byte>{std::byte{0x01}, std::byte{0x02}, std::byte{0x03}}));
    expectUnchanged(output, original);

    PreparedNewRunJournalV1 next;
    ASSERT_EQ(prepareNewRunJournalV1(targetDirectory.catalogPath(), newRunConfiguration(), next).outcome,
              NewRunJournalPreparationOutcome::PREPARED);
    EXPECT_EQ(next.header.exchangeRunId, domain::ExchangeRunId{2});
    EXPECT_EQ(readBytes(targetDirectory.journalPath(1)), evidence);
}

} // namespace
} // namespace exchange::storage
