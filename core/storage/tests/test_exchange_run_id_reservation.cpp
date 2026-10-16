#include "exchange_run_id_reservation.hpp"

#include "exchange_run_id_reservation_internal.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <string>
#include <unistd.h>
#include <vector>

namespace exchange::storage {
namespace {

class ReservationTestDirectory final {
public:
    ReservationTestDirectory() {
        std::array<char, 53> pathTemplate{};
        constexpr char TEMPLATE[] = "/tmp/exchange-run-reservation-test-XXXXXX";
        std::copy(std::begin(TEMPLATE), std::end(TEMPLATE), pathTemplate.begin());
        char *created = ::mkdtemp(pathTemplate.data());
        if (created != nullptr) {
            path_ = created;
        }
    }

    ~ReservationTestDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    ReservationTestDirectory(const ReservationTestDirectory &) = delete;
    ReservationTestDirectory &operator=(const ReservationTestDirectory &) = delete;

    [[nodiscard]] bool valid() const noexcept {
        return !path_.empty();
    }

    [[nodiscard]] const std::filesystem::path &path() const noexcept {
        return path_;
    }

    [[nodiscard]] std::filesystem::path catalogPath() const {
        return path_ / "run-catalog-v1";
    }

private:
    std::filesystem::path path_{};
};

void writeBytes(const std::filesystem::path &path, const std::span<const std::byte> bytes) {
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(stream.is_open());
    stream.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(stream.good());
}

void writeSnapshot(const std::filesystem::path &path, const RunCatalogSnapshotV1 &snapshot) {
    RunCatalogV1Bytes bytes{};
    ASSERT_EQ(encodeRunCatalogV1(snapshot, bytes), RunCatalogCodecError::NONE);
    writeBytes(path, bytes);
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

ssize_t failWrite(void *, int, const void *, std::size_t) noexcept {
    errno = EIO;
    return -1;
}

int replaceThenReportInterruption(void *, const char *source, const char *destination) noexcept {
    if (::rename(source, destination) != 0) {
        return -1;
    }
    errno = EINTR;
    return -1;
}

TEST(ExchangeRunIdReservationTest, FirstReservationCreatesGenerationOneWithRunIdOne) {
    ReservationTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    domain::ExchangeRunId reserved{777};

    const ExchangeRunIdReservationResult result = reserveNextExchangeRunId(directory.catalogPath(), reserved);

    EXPECT_EQ(result.outcome, ExchangeRunIdReservationOutcome::RESERVED);
    EXPECT_EQ(result.catalogLoad.outcome, RunCatalogLoadOutcome::MISSING);
    ASSERT_TRUE(result.catalogReplacement.has_value());
    EXPECT_EQ(result.catalogReplacement->outcome, RunCatalogReplaceOutcome::COMMITTED);
    EXPECT_EQ(reserved, domain::ExchangeRunId{1});

    RunCatalogSnapshotV1 catalog{};
    ASSERT_EQ(loadRunCatalogV1(directory.catalogPath(), catalog).outcome, RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(catalog, (RunCatalogSnapshotV1{
                           .generation = 1,
                           .lastReservedRunId = domain::ExchangeRunId{1},
                           .activeRunId = std::nullopt,
                           .activeDisposition = RunCatalogDisposition::NONE,
                           .retainedStoppedRunId = std::nullopt,
                       }));
}

TEST(ExchangeRunIdReservationTest, ConsecutiveReservationsIncrementGenerationAndRunId) {
    ReservationTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    domain::ExchangeRunId first{777};
    domain::ExchangeRunId second{888};

    ASSERT_EQ(reserveNextExchangeRunId(directory.catalogPath(), first).outcome,
              ExchangeRunIdReservationOutcome::RESERVED);
    const ExchangeRunIdReservationResult result = reserveNextExchangeRunId(directory.catalogPath(), second);

    EXPECT_EQ(result.outcome, ExchangeRunIdReservationOutcome::RESERVED);
    EXPECT_EQ(first, domain::ExchangeRunId{1});
    EXPECT_EQ(second, domain::ExchangeRunId{2});
    RunCatalogSnapshotV1 catalog{};
    ASSERT_EQ(loadRunCatalogV1(directory.catalogPath(), catalog).outcome, RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(catalog.generation, 2U);
    EXPECT_EQ(catalog.lastReservedRunId, domain::ExchangeRunId{2});
}

TEST(ExchangeRunIdReservationTest, ExistingCatalogReservationPreservesLifecycleFieldsExactly) {
    ReservationTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    const RunCatalogSnapshotV1 original{
        .generation = 41,
        .lastReservedRunId = domain::ExchangeRunId{50},
        .activeRunId = domain::ExchangeRunId{48},
        .activeDisposition = RunCatalogDisposition::FAIL_STOPPED,
        .retainedStoppedRunId = domain::ExchangeRunId{49},
    };
    writeSnapshot(directory.catalogPath(), original);
    domain::ExchangeRunId reserved{777};

    ASSERT_EQ(reserveNextExchangeRunId(directory.catalogPath(), reserved).outcome,
              ExchangeRunIdReservationOutcome::RESERVED);

    EXPECT_EQ(reserved, domain::ExchangeRunId{51});
    RunCatalogSnapshotV1 catalog{};
    ASSERT_EQ(loadRunCatalogV1(directory.catalogPath(), catalog).outcome, RunCatalogLoadOutcome::LOADED);
    EXPECT_EQ(catalog.generation, 42U);
    EXPECT_EQ(catalog.lastReservedRunId, domain::ExchangeRunId{51});
    EXPECT_EQ(catalog.activeRunId, original.activeRunId);
    EXPECT_EQ(catalog.activeDisposition, original.activeDisposition);
    EXPECT_EQ(catalog.retainedStoppedRunId, original.retainedStoppedRunId);
}

TEST(ExchangeRunIdReservationTest, MissingCatalogWithJournalLikeEntryFailsClosed) {
    ReservationTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    std::ofstream journal(directory.path() / "run-untrusted.fxjr", std::ios::binary);
    ASSERT_TRUE(journal.is_open());
    journal.close();
    domain::ExchangeRunId reserved{777};

    const ExchangeRunIdReservationResult result = reserveNextExchangeRunId(directory.catalogPath(), reserved);

    EXPECT_EQ(result.outcome, ExchangeRunIdReservationOutcome::JOURNAL_PRESENT_WITHOUT_CATALOG);
    EXPECT_EQ(result.catalogLoad.outcome, RunCatalogLoadOutcome::MISSING);
    EXPECT_FALSE(result.catalogReplacement.has_value());
    EXPECT_EQ(reserved, domain::ExchangeRunId{777});
    EXPECT_FALSE(std::filesystem::exists(directory.catalogPath()));
}

TEST(ExchangeRunIdReservationTest, MalformedAndIoFailedCatalogsPreserveCallerOutput) {
    ReservationTestDirectory malformedDirectory;
    ASSERT_TRUE(malformedDirectory.valid());
    const std::array malformedBytes{std::byte{'F'}, std::byte{'X'}};
    writeBytes(malformedDirectory.catalogPath(), malformedBytes);
    domain::ExchangeRunId malformedOutput{777};

    const ExchangeRunIdReservationResult malformed =
        reserveNextExchangeRunId(malformedDirectory.catalogPath(), malformedOutput);

    EXPECT_EQ(malformed.outcome, ExchangeRunIdReservationOutcome::CATALOG_LOAD_FAILED);
    EXPECT_EQ(malformed.catalogLoad.outcome, RunCatalogLoadOutcome::INVALID);
    EXPECT_EQ(malformed.catalogLoad.codecError, RunCatalogCodecError::INVALID_SIZE);
    EXPECT_EQ(malformedOutput, domain::ExchangeRunId{777});

    ReservationTestDirectory ioDirectory;
    ASSERT_TRUE(ioDirectory.valid());
    domain::ExchangeRunId ioOutput{888};

    const ExchangeRunIdReservationResult ioFailure = reserveNextExchangeRunId(ioDirectory.path(), ioOutput);

    EXPECT_EQ(ioFailure.outcome, ExchangeRunIdReservationOutcome::CATALOG_LOAD_FAILED);
    EXPECT_EQ(ioFailure.catalogLoad.outcome, RunCatalogLoadOutcome::IO_FAILURE);
    EXPECT_EQ(ioFailure.catalogLoad.systemError, EISDIR);
    EXPECT_EQ(ioOutput, domain::ExchangeRunId{888});
}

TEST(ExchangeRunIdReservationTest, GenerationAndRunIdExhaustionDoNotMutateCatalogOrOutput) {
    const auto expectExhausted = [](const RunCatalogSnapshotV1 &snapshot) {
        ReservationTestDirectory directory;
        ASSERT_TRUE(directory.valid());
        writeSnapshot(directory.catalogPath(), snapshot);
        const std::vector<std::byte> originalBytes = readBytes(directory.catalogPath());
        domain::ExchangeRunId reserved{777};

        const ExchangeRunIdReservationResult result = reserveNextExchangeRunId(directory.catalogPath(), reserved);

        EXPECT_EQ(result.outcome, ExchangeRunIdReservationOutcome::EXHAUSTED);
        EXPECT_FALSE(result.catalogReplacement.has_value());
        EXPECT_EQ(reserved, domain::ExchangeRunId{777});
        EXPECT_EQ(readBytes(directory.catalogPath()), originalBytes);
    };

    expectExhausted({
        .generation = std::numeric_limits<std::uint64_t>::max(),
        .lastReservedRunId = domain::ExchangeRunId{3},
        .activeRunId = domain::ExchangeRunId{3},
        .activeDisposition = RunCatalogDisposition::PAUSED,
        .retainedStoppedRunId = std::nullopt,
    });
    expectExhausted({
        .generation = 9,
        .lastReservedRunId = domain::ExchangeRunId{std::numeric_limits<std::uint64_t>::max()},
        .activeRunId = domain::ExchangeRunId{std::numeric_limits<std::uint64_t>::max() - 1},
        .activeDisposition = RunCatalogDisposition::RECOVERY_FAILED,
        .retainedStoppedRunId = domain::ExchangeRunId{std::numeric_limits<std::uint64_t>::max()},
    });
}

TEST(ExchangeRunIdReservationTest, DefiniteReplacementFailurePreservesCallerOutput) {
    ReservationTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto hooks = detail::systemRunCatalogStorageHooks();
    hooks.writeFile = failWrite;
    domain::ExchangeRunId reserved{777};

    const ExchangeRunIdReservationResult result =
        detail::reserveNextExchangeRunIdWithHooks(directory.catalogPath(), hooks, reserved);

    EXPECT_EQ(result.outcome, ExchangeRunIdReservationOutcome::CATALOG_REPLACE_FAILED);
    ASSERT_TRUE(result.catalogReplacement.has_value());
    EXPECT_EQ(result.catalogReplacement->outcome, RunCatalogReplaceOutcome::NOT_COMMITTED_IO_FAILURE);
    EXPECT_EQ(result.catalogReplacement->systemError, EIO);
    EXPECT_EQ(reserved, domain::ExchangeRunId{777});
    EXPECT_FALSE(std::filesystem::exists(directory.catalogPath()));
}

TEST(ExchangeRunIdReservationTest, UncertainReplacementNeverExposesProposedRunId) {
    ReservationTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    auto hooks = detail::systemRunCatalogStorageHooks();
    hooks.replaceFile = replaceThenReportInterruption;
    domain::ExchangeRunId reserved{777};

    const ExchangeRunIdReservationResult result =
        detail::reserveNextExchangeRunIdWithHooks(directory.catalogPath(), hooks, reserved);

    EXPECT_EQ(result.outcome, ExchangeRunIdReservationOutcome::CATALOG_REPLACE_FAILED);
    ASSERT_TRUE(result.catalogReplacement.has_value());
    EXPECT_EQ(result.catalogReplacement->outcome, RunCatalogReplaceOutcome::UNCERTAIN);
    ASSERT_TRUE(result.catalogReplacement->observedSnapshot.has_value());
    EXPECT_EQ(result.catalogReplacement->observedSnapshot->lastReservedRunId, domain::ExchangeRunId{1});
    EXPECT_EQ(reserved, domain::ExchangeRunId{777});
}

TEST(ExchangeRunIdReservationTest, MissingDataDirectoryReportsIoFailureWithoutChangingOutput) {
    ReservationTestDirectory directory;
    ASSERT_TRUE(directory.valid());
    domain::ExchangeRunId reserved{777};
    const std::filesystem::path catalog = directory.path() / "missing" / "run-catalog-v1";

    const ExchangeRunIdReservationResult result = reserveNextExchangeRunId(catalog, reserved);

    EXPECT_EQ(result.outcome, ExchangeRunIdReservationOutcome::DATA_DIRECTORY_IO_FAILURE);
    EXPECT_EQ(result.systemError, ENOENT);
    EXPECT_EQ(reserved, domain::ExchangeRunId{777});
    EXPECT_FALSE(std::filesystem::exists(catalog));
}

} // namespace
} // namespace exchange::storage
