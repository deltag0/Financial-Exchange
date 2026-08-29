#pragma once

#include "../../domain/include/business_events.hpp"
#include "../../domain/include/command_result_fwd.hpp"
#include "../../../sequencer/include/sequence_message.hpp"

#include <cstddef>
#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <variant>

namespace exchange::core::admission {

struct AdmissionKey final {
    domain::ClientId clientId;
    domain::ClientCommandId clientCommandId;

    bool operator==(const AdmissionKey&) const = default;
};

struct NormalizedNewOrder final {
    domain::InstrumentId instrumentId;
    std::uint64_t configurationVersion;
    domain::Side side;
    domain::Price price;
    domain::Quantity quantity;
    task::TimeInForce timeInForce;

    bool operator==(const NormalizedNewOrder&) const = default;
};

struct NormalizedCancel final {
    domain::InstrumentId instrumentId;
    domain::TargetOrderId targetOrderId;

    bool operator==(const NormalizedCancel&) const = default;
};

using NormalizedBusinessCommand = std::variant<NormalizedNewOrder, NormalizedCancel>;

enum class AdmissionStatus : std::uint8_t {
    FIRST_SUBMISSION = 1,
    IDENTICAL_IN_FLIGHT = 2,
    IDENTICAL_COMPLETED = 3,
    CONFLICTING_REUSE = 4,
    ADMISSION_UNAVAILABLE = 5,
};

struct AdmissionDecision final {
    AdmissionStatus status;
    std::optional<domain::AdmissionRejectionReason> rejectionReason;
    matching_engine::ImmutableCommandResultBatch originalResult;
};

struct AdmissionStatistics final {
    std::size_t firstSubmissions{};
    std::size_t identicalInFlight{};
    std::size_t identicalCompleted{};
    std::size_t conflictingReuse{};
    std::size_t admissionUnavailable{};

    bool operator==(const AdmissionStatistics&) const = default;
};

enum class MarkSequencedStatus : std::uint8_t {
    SEQUENCED = 1,
    IDEMPOTENT = 2,
    INVALID_SEQUENCE = 3,
    UNKNOWN_RESERVATION = 4,
    COMMAND_MISMATCH = 5,
    WRONG_STATE = 6,
    SEQUENCE_MISMATCH = 7,
};

enum class CompletionStatus : std::uint8_t {
    COMPLETED = 1,
    INVALID_BATCH = 2,
    UNKNOWN_RESERVATION = 3,
    WRONG_STATE = 4,
    CORRELATION_MISMATCH = 5,
};

class CommandAdmissionIndex final {
public:
    explicit CommandAdmissionIndex(std::size_t capacity);

    [[nodiscard]] AdmissionDecision reserve(const sequencer::sequenceMessage& command);
    [[nodiscard]] MarkSequencedStatus markSequenced(const sequencer::sequenceMessage& command);
    [[nodiscard]] CompletionStatus complete(matching_engine::ImmutableCommandResultBatch result);
    [[nodiscard]] matching_engine::ImmutableCommandResultBatch completedResult(
        const sequencer::sequenceMessage& command) const;
    [[nodiscard]] bool abandonReservation(const sequencer::sequenceMessage& command);

    [[nodiscard]] std::size_t size() const;
    [[nodiscard]] std::size_t capacity() const noexcept;
    [[nodiscard]] AdmissionStatistics statistics() const;

private:
    struct AdmissionKeyLess final {
        bool operator()(const AdmissionKey& left, const AdmissionKey& right) const noexcept;
    };

    enum class RecordState : std::uint8_t {
        RESERVED,
        SEQUENCED,
        COMPLETED,
    };

    struct Record final {
        NormalizedBusinessCommand command;
        RecordState state;
        domain::CommandSequence commandSequence;
        matching_engine::ImmutableCommandResultBatch result;
    };

    static AdmissionKey keyFrom(const sequencer::sequenceMessage& command);
    static AdmissionKey keyFrom(const domain::CommandResultCorrelation& correlation);
    static NormalizedBusinessCommand businessCommandFrom(const sequencer::sequenceMessage& command);

    const std::size_t capacity_;
    mutable std::mutex mutex_;
    std::map<AdmissionKey, Record, AdmissionKeyLess> records_;
    AdmissionStatistics statistics_;
};

} // namespace exchange::core::admission
