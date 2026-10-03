#include "run_journal_reader.hpp"

#include "journal_v1_envelope.hpp"
#include "little_endian.hpp"
#include "run_journal_reader_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <new>
#include <span>
#include <unistd.h>
#include <utility>

namespace exchange::storage {
namespace {

constexpr std::size_t RUN_HEADER_MAX_FRAME_SIZE = 65'536;

ssize_t systemReadFile(void *, const int descriptor, void *buffer, const std::size_t size) noexcept {
    return ::read(descriptor, buffer, size);
}

const detail::RunJournalReaderHooks SYSTEM_HOOKS{
    nullptr,
    systemReadFile,
};

struct ReadResult final {
    std::size_t bytesRead{0};
    int systemError{0};
};

RunJournalLoadResult loadResult(const RunJournalLoadOutcome outcome,
                                const RunJournalLoadError error = RunJournalLoadError::NONE,
                                const RunHeaderCodecError headerCodecError = RunHeaderCodecError::NONE,
                                const JournalCommandCodecError commandCodecError = JournalCommandCodecError::NONE,
                                const int systemError = 0, const std::uint64_t tailOffset = 0,
                                const std::uint64_t tailLength = 0) noexcept {
    return {outcome, error, headerCodecError, commandCodecError, systemError, tailOffset, tailLength};
}

RunJournalLoadResult ioFailure(const int systemError,
                               const RunHeaderCodecError headerCodecError = RunHeaderCodecError::NONE) noexcept {
    return loadResult(RunJournalLoadOutcome::IO_FAILURE, RunJournalLoadError::NONE, headerCodecError,
                      JournalCommandCodecError::NONE, systemError);
}

RunJournalLoadResult headerCorruption(const RunHeaderCodecError codecError) noexcept {
    return loadResult(RunJournalLoadOutcome::CORRUPTION, RunJournalLoadError::NONE, codecError);
}

RunJournalLoadResult commandCorruption(const JournalCommandCodecError codecError) noexcept {
    return loadResult(RunJournalLoadOutcome::CORRUPTION, RunJournalLoadError::NONE, RunHeaderCodecError::NONE,
                      codecError);
}

ReadResult readSome(const int descriptor, const std::span<std::byte> destination,
                    const detail::RunJournalReaderHooks &hooks) noexcept {
    while (true) {
        const ssize_t result = hooks.readFile(hooks.context, descriptor, destination.data(), destination.size());
        if (result > 0 && static_cast<std::size_t>(result) <= destination.size()) {
            return {static_cast<std::size_t>(result), 0};
        }
        if (result == 0) {
            return {0, 0};
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return {0, result > 0 ? EIO : errno};
    }
}

ReadResult readExact(const int descriptor, const std::span<std::byte> destination,
                     const detail::RunJournalReaderHooks &hooks) noexcept {
    std::size_t bytesRead = 0;
    while (bytesRead < destination.size()) {
        const auto result = readSome(descriptor, destination.subspan(bytesRead), hooks);
        if (result.systemError != 0) {
            return {bytesRead, result.systemError};
        }
        if (result.bytesRead == 0) {
            return {bytesRead, 0};
        }
        bytesRead += result.bytesRead;
    }
    return {bytesRead, 0};
}

bool exceedsCapacity(const std::uint64_t currentBytes, const std::size_t additionalBytes,
                     const std::uint64_t maximumBytes) noexcept {
    return currentBytes > maximumBytes || additionalBytes > maximumBytes - currentBytes;
}

RunJournalLoadError validateCommandConfiguration(const RunHeaderV1 &header, const domain::InstrumentId instrumentId,
                                                 const std::uint32_t configurationVersion) noexcept {
    const auto instrument = std::lower_bound(
        header.instruments.begin(), header.instruments.end(), instrumentId,
        [](const RunHeaderInstrumentV1 &entry, const domain::InstrumentId id) { return entry.instrumentId < id; });
    if (instrument == header.instruments.end() || instrument->instrumentId != instrumentId ||
        instrument->configurationVersion != configurationVersion) {
        return RunJournalLoadError::INSTRUMENT_CONFIGURATION_MISMATCH;
    }
    return RunJournalLoadError::NONE;
}

JournalNewOrderV1 newOrderDecodeTarget() {
    return {
        .exchangeRunId = domain::ExchangeRunId{1},
        .commandSequence = domain::CommandSequence{1},
        .behavioralRulesVersion = 1,
        .configurationVersion = 1,
        .clientId = domain::ClientId{1},
        .instrumentId = domain::InstrumentId{1},
        .clientCommandId = domain::ClientCommandId{"x"},
        .side = domain::Side::BUY,
        .timeInForce = core::task::TimeInForce::GTC,
        .price = domain::Price{1},
        .quantity = domain::Quantity{1},
    };
}

JournalCancelV1 cancelDecodeTarget() {
    return {
        .exchangeRunId = domain::ExchangeRunId{1},
        .commandSequence = domain::CommandSequence{1},
        .behavioralRulesVersion = 1,
        .configurationVersion = 1,
        .clientId = domain::ClientId{1},
        .instrumentId = domain::InstrumentId{1},
        .clientCommandId = domain::ClientCommandId{"x"},
        .targetOrderId = domain::TargetOrderId{1},
    };
}

RunJournalLoadResult readJournal(const int descriptor, const detail::RunJournalReaderHooks &hooks,
                                 LoadedRunJournalV1 &decoded) noexcept {
    std::array<std::byte, detail::JOURNAL_V1_ENVELOPE_SIZE> envelope{};
    const auto headerEnvelopeRead = readExact(descriptor, envelope, hooks);
    if (headerEnvelopeRead.systemError != 0) {
        return ioFailure(headerEnvelopeRead.systemError);
    }
    if (headerEnvelopeRead.bytesRead != envelope.size()) {
        return headerCorruption(RunHeaderCodecError::TRUNCATED_INPUT);
    }

    const std::uint32_t headerLength = detail::readUint32LittleEndian(envelope, detail::JOURNAL_V1_TOTAL_LENGTH_OFFSET);
    if (headerLength < detail::JOURNAL_V1_ENVELOPE_SIZE) {
        return headerCorruption(RunHeaderCodecError::INVALID_TOTAL_LENGTH);
    }
    if (headerLength > RUN_HEADER_MAX_FRAME_SIZE) {
        return headerCorruption(RunHeaderCodecError::FRAME_TOO_LARGE);
    }

    std::vector<std::byte> headerFrame;
    try {
        headerFrame.resize(headerLength);
    } catch (const std::bad_alloc &) {
        return ioFailure(ENOMEM);
    }
    std::copy(envelope.begin(), envelope.end(), headerFrame.begin());
    const auto headerPayloadRead =
        readExact(descriptor, std::span<std::byte>(headerFrame).subspan(detail::JOURNAL_V1_ENVELOPE_SIZE), hooks);
    if (headerPayloadRead.systemError != 0) {
        return ioFailure(headerPayloadRead.systemError);
    }
    if (headerPayloadRead.bytesRead != headerLength - detail::JOURNAL_V1_ENVELOPE_SIZE) {
        return headerCorruption(RunHeaderCodecError::TRUNCATED_INPUT);
    }

    RunHeaderV1 header;
    const auto headerError = decodeRunHeaderV1(headerFrame, header);
    if (headerError != RunHeaderCodecError::NONE) {
        if (headerError == RunHeaderCodecError::ALLOCATION_FAILURE) {
            return ioFailure(ENOMEM, headerError);
        }
        return headerCorruption(headerError);
    }

    decoded.header = std::move(header);
    decoded.validCommittedByteCount = headerLength;
    std::uint64_t expectedSequence = 1;

    while (true) {
        envelope.fill(std::byte{0});
        const auto firstEnvelopeRead = readSome(descriptor, envelope, hooks);
        if (firstEnvelopeRead.systemError != 0) {
            return ioFailure(firstEnvelopeRead.systemError);
        }
        if (firstEnvelopeRead.bytesRead == 0) {
            return loadResult(RunJournalLoadOutcome::VALID);
        }
        if (decoded.commands.size() >= decoded.header.maxRunCommands) {
            return loadResult(RunJournalLoadOutcome::CORRUPTION, RunJournalLoadError::COMMAND_COUNT_CAPACITY_EXCEEDED);
        }
        const auto remainingEnvelopeRead =
            readExact(descriptor, std::span<std::byte>(envelope).subspan(firstEnvelopeRead.bytesRead), hooks);
        if (remainingEnvelopeRead.systemError != 0) {
            return ioFailure(remainingEnvelopeRead.systemError);
        }
        const std::size_t envelopeBytesRead = firstEnvelopeRead.bytesRead + remainingEnvelopeRead.bytesRead;
        if (envelopeBytesRead < envelope.size()) {
            if (exceedsCapacity(decoded.validCommittedByteCount, envelopeBytesRead,
                                decoded.header.maxRunJournalBytes)) {
                return loadResult(RunJournalLoadOutcome::CORRUPTION, RunJournalLoadError::FILE_BYTE_CAPACITY_EXCEEDED);
            }
            return loadResult(RunJournalLoadOutcome::INCOMPLETE_TAIL, RunJournalLoadError::NONE,
                              RunHeaderCodecError::NONE, JournalCommandCodecError::NONE, 0,
                              decoded.validCommittedByteCount, envelopeBytesRead);
        }

        detail::JournalV1CommandEnvelope commandEnvelope;
        const auto envelopeError = detail::validateJournalV1CommandEnvelope(envelope, 0, false, commandEnvelope);
        if (envelopeError != JournalCommandCodecError::NONE) {
            return commandCorruption(envelopeError);
        }
        if (domain::ExchangeRunId{commandEnvelope.exchangeRunId} != decoded.header.exchangeRunId) {
            return loadResult(RunJournalLoadOutcome::CORRUPTION, RunJournalLoadError::EXCHANGE_RUN_ID_MISMATCH);
        }
        if (commandEnvelope.commandSequence != expectedSequence) {
            return loadResult(RunJournalLoadOutcome::CORRUPTION, RunJournalLoadError::COMMAND_SEQUENCE_MISMATCH);
        }
        if (exceedsCapacity(decoded.validCommittedByteCount, commandEnvelope.totalLength,
                            decoded.header.maxRunJournalBytes)) {
            return loadResult(RunJournalLoadOutcome::CORRUPTION, RunJournalLoadError::FILE_BYTE_CAPACITY_EXCEEDED);
        }

        std::array<std::byte, detail::JOURNAL_V1_COMMAND_MAX_FRAME_SIZE> commandFrame{};
        std::copy(envelope.begin(), envelope.end(), commandFrame.begin());
        const auto payload = std::span<std::byte>(commandFrame)
                                 .subspan(detail::JOURNAL_V1_ENVELOPE_SIZE,
                                          commandEnvelope.totalLength - detail::JOURNAL_V1_ENVELOPE_SIZE);
        const auto payloadRead = readExact(descriptor, payload, hooks);
        if (payloadRead.systemError != 0) {
            return ioFailure(payloadRead.systemError);
        }
        const std::size_t physicalFrameBytes = detail::JOURNAL_V1_ENVELOPE_SIZE + payloadRead.bytesRead;
        if (payloadRead.bytesRead != payload.size()) {
            return loadResult(RunJournalLoadOutcome::INCOMPLETE_TAIL, RunJournalLoadError::NONE,
                              RunHeaderCodecError::NONE, JournalCommandCodecError::NONE, 0,
                              decoded.validCommittedByteCount, physicalFrameBytes);
        }
        const auto frame = std::span<const std::byte>(commandFrame).first(commandEnvelope.totalLength);
        RunJournalLoadError contextError = RunJournalLoadError::NONE;
        try {
            if (commandEnvelope.recordType == detail::JOURNAL_V1_NEW_ORDER_RECORD_TYPE) {
                auto command = newOrderDecodeTarget();
                const auto codecError = decodeNewOrderV1(frame, command);
                if (codecError != JournalCommandCodecError::NONE) {
                    return commandCorruption(codecError);
                }
                contextError =
                    validateCommandConfiguration(decoded.header, command.instrumentId, command.configurationVersion);
                if (contextError == RunJournalLoadError::NONE) {
                    decoded.commands.emplace_back(std::move(command));
                }
            } else {
                auto command = cancelDecodeTarget();
                const auto codecError = decodeCancelV1(frame, command);
                if (codecError != JournalCommandCodecError::NONE) {
                    return commandCorruption(codecError);
                }
                contextError =
                    validateCommandConfiguration(decoded.header, command.instrumentId, command.configurationVersion);
                if (contextError == RunJournalLoadError::NONE) {
                    decoded.commands.emplace_back(std::move(command));
                }
            }
        } catch (const std::bad_alloc &) {
            return ioFailure(ENOMEM);
        }
        if (contextError != RunJournalLoadError::NONE) {
            return loadResult(RunJournalLoadOutcome::CORRUPTION, contextError);
        }

        decoded.validCommittedByteCount += commandEnvelope.totalLength;
        ++expectedSequence;
    }
}

int openReadOnly(const std::filesystem::path &path) noexcept {
    int descriptor = -1;
    do {
        descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
    } while (descriptor < 0 && errno == EINTR);
    return descriptor;
}

} // namespace

namespace detail {

RunJournalLoadResult loadRunJournalV1FromDescriptorWithHooks(const int descriptor, const RunJournalReaderHooks &hooks,
                                                             LoadedRunJournalV1 &output) noexcept {
    LoadedRunJournalV1 decoded;
    const auto result = readJournal(descriptor, hooks, decoded);
    if (result.outcome == RunJournalLoadOutcome::VALID || result.outcome == RunJournalLoadOutcome::INCOMPLETE_TAIL) {
        output = std::move(decoded);
    }
    return result;
}

RunJournalLoadResult loadRunJournalV1WithHooks(const std::filesystem::path &canonicalPath,
                                               const RunJournalReaderHooks &hooks,
                                               LoadedRunJournalV1 &output) noexcept {
    const int descriptor = openReadOnly(canonicalPath);
    if (descriptor < 0) {
        const int systemError = errno;
        if (systemError == ENOENT) {
            return loadResult(RunJournalLoadOutcome::MISSING, RunJournalLoadError::NONE, RunHeaderCodecError::NONE,
                              JournalCommandCodecError::NONE, systemError);
        }
        return ioFailure(systemError);
    }

    LoadedRunJournalV1 decoded;
    const auto result = loadRunJournalV1FromDescriptorWithHooks(descriptor, hooks, decoded);
    const int closeResult = ::close(descriptor);
    if (closeResult != 0 &&
        (result.outcome == RunJournalLoadOutcome::VALID || result.outcome == RunJournalLoadOutcome::INCOMPLETE_TAIL)) {
        return ioFailure(errno);
    }

    if (result.outcome == RunJournalLoadOutcome::VALID || result.outcome == RunJournalLoadOutcome::INCOMPLETE_TAIL) {
        output = std::move(decoded);
    }
    return result;
}

} // namespace detail

RunJournalLoadResult loadRunJournalV1(const std::filesystem::path &canonicalPath, LoadedRunJournalV1 &output) noexcept {
    return detail::loadRunJournalV1WithHooks(canonicalPath, SYSTEM_HOOKS, output);
}

} // namespace exchange::storage
