// Definitions for Bus template methods. Kept in src but included by header at compile time.
#include "../include/bus.hpp"

namespace exchange::core {

template <typename T> template <typename U> bool Bus<T>::writeImpl(U &&value) {
    const cursor_type current_write = write_sequence_.load(std::memory_order_relaxed);

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

    buffer_[static_cast<std::size_t>(current_write % size_)] = std::forward<U>(value);
    write_sequence_.store(current_write + 1, std::memory_order_release);
    return true;
}

template <typename T> typename Bus<T>::cursor_type Bus<T>::recomputeMinimumCursor() const {
    const cursor_type current_write = write_sequence_.load(std::memory_order_acquire);
    cursor_type minimum_cursor = current_write;

    for (const std::atomic<cursor_type> *cursor : reader_cursors_) {
        if (cursor == nullptr) {
            continue;
        }

        minimum_cursor = std::min(minimum_cursor, cursor->load(std::memory_order_acquire));
    }

    return minimum_cursor;
}

} // namespace exchange::core

// Important: This file contains template definitions and must be included where used.