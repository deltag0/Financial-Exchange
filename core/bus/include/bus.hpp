#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
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

    void cleanJournal() {
        uint64_t min_cursor = cached_min_cursor_.load();
        std::string log;
        int journalLine;

        std::getline(journalFile, log);

        std::istringstream log_stream{log};
        log_stream.ignore(std::numeric_limits<std::streamsize>::max(), '#');
        log_stream >> journalLine; 

        if (journalLine < min_cursor) {

        }







    }

    // Read from the index of the cursor first
    bool read(std::atomic<cursor_type> &cursor, sequencer::sequenceMessage *output) {
        const cursor_type current_cursor = cursor.load(std::memory_order_acquire);
        const cursor_type next_write = total_writes_.load(std::memory_order_acquire);

        if (next_write - size_ > current_cursor) {
            // check how many times we wrote over 
            std::uint64_t times_overwritten{(next_write - current_cursor) / size_};
            cached_min_cursor_.store(recomputeMinimumCursor());
            uint64_t min_cursor = cached_min_cursor_.load();
            bool updatedPos;
            int currLine = INT_MIN;
            std::string log;
            std::streampos new_start;

            std::uint64_t lines_to_read{times_overwritten * size_ + 1};
            output = new sequencer::sequenceMessage(times_overwritten);

            // We have a gurantee that the file pointer will always be before or starting at the min cursor
            while (currLine + 1 < current_cursor) {
                std::getline(journalFile, log);
                std::istringstream log_stream{log};

                log_stream.ignore(std::numeric_limits<std::streamsize>::max(), '#');
                log_stream >> currLine;

                // currLine will always be before or after/at the min cursor
                // so we can looking ahead by 1 is fine to find a new start
                if (currLine + 1 == min_cursor) {
                    new_start = journalFile.tellg();

                }
            }


            for (int i{0}; i < lines_to_read; ++i) {
                std::getline(journalFile, log);

                std::istringstream log_stream{log};

                log_stream.ignore(std::numeric_limits<std::streamsize>::max(), '#');
                log_stream >> currLine;

                log_stream.ignore(std::numeric_limits<std::streamsize>::max(), ':');
                log = log_stream.str();

                output[i] = sequencer::sequenceMessage::jsonToSequenceMessage(log);
            }

            cursor.store(next_write + 1, std::memory_order_release);
            journalFile.seekg(new_start);
            return false;
        }

        const cursor_type oldest_available = next_write > size_ ? next_write - size_ : 0;
        if (current_cursor < oldest_available) {
            return false;
        }

        const std::size_t index = static_cast<std::size_t>(current_cursor % size_);
        std::optional<sequencer::sequenceMessage>* slot = &buffer_[index];
        if (!slot->has_value()) {
            return false;
        }

        output = &slot->value();
        cursor.store(current_cursor + 1, std::memory_order_release);
        return true;
    }

    bool read(std::atomic<cursor_type>& cursor, sequencer::sequenceMessage& output) {
        const cursor_type current_cursor = cursor.load(std::memory_order_acquire);
        const cursor_type next_write = total_writes_.load(std::memory_order_acquire);
        if (current_cursor >= next_write) {
            return false;
        }

        const cursor_type oldest_available = next_write > size_ ? next_write - size_ : 0;
        if (current_cursor < oldest_available) {
            return false;
        }

        const std::size_t index = static_cast<std::size_t>(current_cursor % size_);
        const std::optional<sequencer::sequenceMessage>& slot = buffer_[index];
        if (!slot.has_value()) {
            return false;
        }

        output = slot.value();
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
        

        // might only need to write if we need to (forgot the conditions)
        journalFile << "Write #" << total_writes_.load(std::memory_order_acquire) << " : " << message.toJson() << "\n";
        return true;
    }

    /*
    Note that I think there would be a way to always keep track of the minimum cursor using a linked list and map with the pointers poitning to their node
    then on a read, it's an O(1) operation to update the minimum cursor
    */
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
