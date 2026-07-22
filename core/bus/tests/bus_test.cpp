#include "../include/bus.hpp"
#include <atomic>
#include <cstring>
#include <gtest/gtest.h>
#include <thread>

namespace exchange::core::test {

namespace {
// Build a sequenceMessage carrying an identifying id so tests can assert ordering/identity.
sequencer::sequenceMessage msg(uint64_t id) {
    sequencer::sequenceMessage m{};
    m.id = id;
    return m;
}
} // namespace

// Test basic write and read operations
TEST(BusTest, BasicWriteRead) {
    Bus bus(10);
    std::atomic<Bus::cursor_type> cursor(0);
    bus.registerCursor(cursor);

    // Write a value
    EXPECT_TRUE(bus.write(msg(42)));

    // Read it back
    sequencer::sequenceMessage value{};
    EXPECT_TRUE(bus.read(cursor, value));
    EXPECT_EQ(value.id, 42u);
    EXPECT_EQ(cursor.load(), 1u);
}

// Test multiple writes and reads in sequence
TEST(BusTest, SequentialWriteRead) {
    Bus bus(10);
    std::atomic<Bus::cursor_type> cursor(0);
    bus.registerCursor(cursor);

    // Write multiple values
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(bus.write(msg(i * 10)));
    }

    // Read them back
    for (int i = 0; i < 5; ++i) {
        sequencer::sequenceMessage value{};
        EXPECT_TRUE(bus.read(cursor, value));
        EXPECT_EQ(value.id, static_cast<uint64_t>(i * 10));
    }
}

// Test that read returns false when cursor is at or ahead of write pointer
TEST(BusTest, ReadEmptyBuffer) {
    Bus bus(10);
    std::atomic<Bus::cursor_type> cursor(0);
    bus.registerCursor(cursor);

    sequencer::sequenceMessage value{};
    EXPECT_FALSE(bus.read(cursor, value));
}

// Test circular wrap-around behavior with proper cursor management
TEST(BusTest, WrapAroundWrite) {
    Bus bus(4); // Small size to force wrap-around
    std::atomic<Bus::cursor_type> cursor(0);
    bus.registerCursor(cursor);

    // Write exactly capacity
    for (int i = 0; i < 4; ++i) {
        EXPECT_TRUE(bus.write(msg(i)));
    }

    // Read them all
    for (int i = 0; i < 4; ++i) {
        sequencer::sequenceMessage value{};
        EXPECT_TRUE(bus.read(cursor, value));
        EXPECT_EQ(value.id, static_cast<uint64_t>(i));
    }

    // Now we can write more (cursor advanced)
    for (int i = 4; i < 8; ++i) {
        EXPECT_TRUE(bus.write(msg(i)));
    }

    // Read the new ones
    for (int i = 4; i < 8; ++i) {
        sequencer::sequenceMessage value{};
        EXPECT_TRUE(bus.read(cursor, value));
        EXPECT_EQ(value.id, static_cast<uint64_t>(i));
    }
}

// Test multiple readers with different cursors
TEST(BusTest, MultipleReaders) {
    Bus bus(10);
    std::atomic<Bus::cursor_type> cursor1(0);
    std::atomic<Bus::cursor_type> cursor2(0);

    bus.registerCursor(cursor1);
    bus.registerCursor(cursor2);

    // Write some values
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(bus.write(msg(i)));
    }

    // Reader 1 reads first 3 values
    for (int i = 0; i < 3; ++i) {
        sequencer::sequenceMessage value{};
        EXPECT_TRUE(bus.read(cursor1, value));
        EXPECT_EQ(value.id, static_cast<uint64_t>(i));
    }

    // Reader 2 reads all 5 values
    for (int i = 0; i < 5; ++i) {
        sequencer::sequenceMessage value{};
        EXPECT_TRUE(bus.read(cursor2, value));
        EXPECT_EQ(value.id, static_cast<uint64_t>(i));
    }

    // Cursors should be at different positions
    EXPECT_EQ(cursor1.load(), 3u);
    EXPECT_EQ(cursor2.load(), 5u);
}

// Test that writer waits for slowest reader before overwriting
TEST(BusTest, OverwriteProtection) {
    Bus bus(5); // Very small buffer
    std::atomic<Bus::cursor_type> cursor1(0);
    std::atomic<Bus::cursor_type> cursor2(0);

    bus.registerCursor(cursor1);
    bus.registerCursor(cursor2);

    // Write up to capacity
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(bus.write(msg(i)));
    }

    // Reader 1 advances
    sequencer::sequenceMessage value{};
    bus.read(cursor1, value);
    bus.read(cursor1, value);
    // cursor1 is now at 2, but cursor2 is at 0

    // Attempt to write more - should fail because cursor2 hasn't advanced
    // and buffer would overwrite data it hasn't read
    for (int i = 5; i < 7; ++i) {
        EXPECT_FALSE(bus.write(msg(i)));
    }

    // Reader 2 advances - now writes should succeed
    for (int j = 0; j < 5; ++j) {
        bus.read(cursor2, value);
    }

    // Now we can write again (cursor2 caught up)
    EXPECT_TRUE(bus.write(msg(100)));
}

// Test with all fields of the message populated
TEST(BusTest, ComplexDataType) {
    Bus bus(10);
    std::atomic<Bus::cursor_type> cursor(0);
    bus.registerCursor(cursor);

    sequencer::sequenceMessage m{};
    m.id = 12345;
    m.price = 999;
    strcpy(m.symbol, "hello");

    EXPECT_TRUE(bus.write(m));

    sequencer::sequenceMessage read_msg{};
    EXPECT_TRUE(bus.read(cursor, read_msg));
    EXPECT_EQ(read_msg.id, 12345u);
    EXPECT_EQ(read_msg.price, 999u);
    EXPECT_STREQ(read_msg.symbol, "hello");
}

// Test that flow control prevents writers when readers fall behind
TEST(BusTest, StaleCursorRejection) {
    Bus bus(5);
    std::atomic<Bus::cursor_type> slow_cursor(0);

    bus.registerCursor(slow_cursor);

    // Fill the buffer to capacity
    for (int i = 0; i < 5; ++i) {
        EXPECT_TRUE(bus.write(msg(i))) << "Failed to write item " << i;
    }

    // Try to write more - should fail because slow_cursor hasn't advanced
    // and buffer is full
    bool write_blocked = false;
    for (int i = 5; i < 10; ++i) {
        if (!bus.write(msg(i))) {
            write_blocked = true;
            break;
        }
    }
    EXPECT_TRUE(write_blocked) << "Expected writes to be blocked, but they weren't";

    // Verify that slow_cursor can still read what's in the buffer
    sequencer::sequenceMessage value{};
    EXPECT_TRUE(bus.read(slow_cursor, value));
    EXPECT_EQ(value.id, 0u);
}

// Test concurrent write and read
TEST(BusTest, ConcurrentReadWrite) {
    Bus bus(100);
    std::atomic<Bus::cursor_type> cursor(0);
    bus.registerCursor(cursor);

    std::atomic<bool> stop_writing{false};
    std::atomic<int> write_count{0};
    std::atomic<int> read_count{0};

    // Writer thread
    std::thread writer([&]() {
        for (int i = 0; i < 1000; ++i) {
            while (!bus.write(msg(i))) {
                std::this_thread::yield();
            }
            write_count.fetch_add(1);
        }
        stop_writing.store(true);
    });

    // Reader thread
    std::thread reader([&]() {
        while (!stop_writing.load() || cursor.load() < 1000) {
            sequencer::sequenceMessage value{};
            if (bus.read(cursor, value)) {
                read_count.fetch_add(1);
            } else {
                std::this_thread::yield();
            }
        }
    });

    writer.join();
    reader.join();

    EXPECT_EQ(write_count.load(), 1000);
    EXPECT_EQ(read_count.load(), 1000);
}

// Test capacity query
TEST(BusTest, CapacityQuery) {
    Bus bus(256);
    EXPECT_EQ(bus.capacity(), 256u);
}

// Test that zero-size bus throws
TEST(BusTest, ZeroSizeThrows) { EXPECT_THROW(Bus bus(0), std::invalid_argument); }

// Test with move semantics
TEST(BusTest, MoveSemantics) {
    Bus bus(10);
    std::atomic<Bus::cursor_type> cursor(0);
    bus.registerCursor(cursor);

    sequencer::sequenceMessage m{};
    m.id = 7;
    strcpy(m.symbol, "AAPL");
    EXPECT_TRUE(bus.write(std::move(m)));

    sequencer::sequenceMessage read_msg{};
    EXPECT_TRUE(bus.read(cursor, read_msg));
    EXPECT_EQ(read_msg.id, 7u);
    EXPECT_STREQ(read_msg.symbol, "AAPL");
}

// Test general behavior with multiple cursors
TEST(BusTest, generalTest) {
    Bus bus(10);

    std::atomic<Bus::cursor_type> cursor1(0);
    std::atomic<Bus::cursor_type> cursor2(0);
    std::atomic<Bus::cursor_type> cursor3(0);

    bus.registerCursor(cursor1);
    bus.registerCursor(cursor2);
    bus.registerCursor(cursor3);

    bool check = false;
    sequencer::sequenceMessage buf{};

    check = bus.read(cursor1, buf);
    EXPECT_FALSE(check);

    // ids 0..9 stand in for "msg0".."msg9"
    for (int i = 0; i < 10; ++i) {
        check = bus.write(msg(i));
        EXPECT_TRUE(check);
    }

    check = bus.write(msg(999)); // overflow
    EXPECT_FALSE(check);

    bus.read(cursor1, buf);
    EXPECT_EQ(buf.id, 0u);
    bus.read(cursor2, buf);
    EXPECT_EQ(buf.id, 0u);
    bus.read(cursor3, buf);
    EXPECT_EQ(buf.id, 0u);

    check = bus.write(msg(10)); // "wrap0"
    EXPECT_TRUE(check);

    for (int i = 0; i < 9; ++i) {
        bus.read(cursor1, buf);
        EXPECT_EQ(buf.id, static_cast<uint64_t>(i + 1));
        bus.read(cursor2, buf);
        EXPECT_EQ(buf.id, static_cast<uint64_t>(i + 1));
        bus.read(cursor3, buf);
        EXPECT_EQ(buf.id, static_cast<uint64_t>(i + 1));
    }

    check = bus.write(msg(11)); // "wrap1"
    EXPECT_TRUE(check);
    bus.read(cursor2, buf);
    EXPECT_EQ(buf.id, 10u); // "wrap0"
    bus.read(cursor3, buf);
    EXPECT_EQ(buf.id, 10u);

    for (int i = 0; i < 8; i++) {
        check = bus.write(msg(12 + i)); // "wrap2".."wrap9"
        EXPECT_TRUE(check);
    }

    check = bus.write(msg(9999)); // "bad"
    EXPECT_FALSE(check);
    EXPECT_EQ(bus.getMinCursor(), 10);
}

} // namespace exchange::core::test

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
