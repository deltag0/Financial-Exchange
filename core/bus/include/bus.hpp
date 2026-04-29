#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace exchange::core {

template <typename T> class Bus {
  public:
    using cursor_type = std::uint64_t;

    explicit Bus(std::size_t size = 1024) : size_(size), buffer_(size) {
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

    bool write(const T &value) { return writeImpl(value); }

    bool write(T &&value) { return writeImpl(std::move(value)); }

    bool read(std::atomic<cursor_type> &cursor, T &output) const {
        const cursor_type current_cursor = cursor.load(std::memory_order_acquire);
        const cursor_type next_write = write_sequence_.load(std::memory_order_acquire);

        if (current_cursor >= next_write) {
            return false;
        }

        const cursor_type oldest_available = next_write > size_ ? next_write - size_ : 0;
        if (current_cursor < oldest_available) {
            return false;
        }

        const std::size_t index = static_cast<std::size_t>(current_cursor % size_);
        const std::optional<T> &slot = buffer_[index];
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
    template <typename U> bool writeImpl(U &&value);

    cursor_type recomputeMinimumCursor() const;

  private:
    std::size_t size_;
    // Circular buffer storing data
    std::vector<std::optional<T>> buffer_;
    // Pointers to read cursors for access during writes for checks
    std::vector<std::atomic<cursor_type> *> reader_cursors_;
    // Cached min cursor guaranteed to be <= all read cursors, updated on writes to avoid
    // recomputation
    std::atomic<cursor_type> cached_min_cursor_{0};
    std::atomic<cursor_type> write_sequence_{0};
};

} // namespace exchange::core

#include "../src/bus.cpp"
