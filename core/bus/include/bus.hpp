#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>
#include <fstream>

#include "../../../sequencer/include/sequence_message.hpp"

namespace exchange::core {

class Bus {
  public:
    using cursor_type = std::uint64_t;

    explicit Bus(std::size_t size = 1024, std::string journalFileName = "") : size_(size), buffer_(size), journalFile{journalFileName} {
        if (size_ == 0) {
            throw std::invalid_argument("Bus size must be greater than zero");
        }
    }

    std::size_t capacity() const noexcept { return size_; }

    void registerCursor(std::atomic<cursor_type> &cursor) {
        reader_cursors_.push_back(&cursor);
        cached_min_cursor_.store(recomputeMinimumCursor(), std::memory_order_release);
    }

    int getMinCursor() const {
        return static_cast<int>(cached_min_cursor_.load(std::memory_order_acquire));
    }

    bool write(const sequencer::sequenceMessage &value) { return writeImpl(value); }

    bool write(sequencer::sequenceMessage &&value) { return writeImpl(std::move(value)); }

    bool read(std::atomic<cursor_type> &cursor, sequencer::sequenceMessage &output) const {
        const cursor_type current_cursor = cursor.load(std::memory_order_acquire);
        const cursor_type next_write = total_writes_.load(std::memory_order_acquire);

        if (next_write - size_ > current_cursor) {
            // check how many times we wrote over 
            std::uint64_t times_overwritten{(next_write - current_cursor) / size_};

            std::uint64_t lines{times_overwritten * size_};

        }

        if (current_cursor >= next_write) {
            return false;
        }

        const cursor_type oldest_available = next_write > size_ ? next_write - size_ : 0;
        if (current_cursor < oldest_available) {
            return false;
        }

        const std::size_t index = static_cast<std::size_t>(current_cursor % size_);
        const std::optional<sequencer::sequenceMessage> &slot = buffer_[index];
        if (!slot.has_value()) {
            return false;
        }

        output = *slot;
        cursor.store(current_cursor + 1, std::memory_order_release);
        return true;
    }

  private:
    /*
     * Core write implementation with perfect forwarding for both lvalue and rvalue.
     * Checks if the next write would overwrite unread data by comparing the current write
     * sequence against the minimum reader cursor. Uses a cached minimum cursor to avoid
     * recomputation on every write, but will recompute if the next write would exceed the
     * cached minimum + size.
     */
        bool writeImpl(const sequencer::sequenceMessage &message) {
        const cursor_type current_write = total_writes_.load(std::memory_order_relaxed);

        if (!reader_cursors_.empty()) {
            cursor_type cached_min = cached_min_cursor_.load(std::memory_order_acquire);
            if (current_write >= cached_min + size_) {
                cached_min = recomputeMinimumCursor();
                cached_min_cursor_.store(cached_min, std::memory_order_release);

                if (current_write >= cached_min + size_) {
                    return false;
                }
            }
        }

        buffer_[static_cast<std::size_t>(current_write % size_)] = message;
        total_writes_.store(current_write + 1, std::memory_order_release);
        journalFile << "Write #" << total_writes_.load(std::memory_order_acquire) << ": " << message.toJson() << "\n";
        return true;
    }

    cursor_type recomputeMinimumCursor() const {
        const cursor_type current_write = total_writes_.load(std::memory_order_acquire);
        cursor_type minimum_cursor = current_write;

        for (const std::atomic<cursor_type> *cursor : reader_cursors_) {
            if (cursor == nullptr) {
                continue;
            }

            minimum_cursor = std::min(minimum_cursor, cursor->load(std::memory_order_acquire));
        }

        return minimum_cursor;
    }

  private:
    std::size_t size_;
    // Circular buffer storing data
    std::vector<std::optional<sequencer::sequenceMessage>> buffer_;
    // Pointers to read cursors for access during writes for checks
    std::vector<std::atomic<cursor_type> *> reader_cursors_;
    // Cached min cursor guaranteed to be <= all read cursors, updated on writes to avoid
    // recomputation
    std::atomic<cursor_type> cached_min_cursor_{0};
    std::atomic<cursor_type> total_writes_{0};

    std::fstream journalFile;
};

} // namespace exchange::core
