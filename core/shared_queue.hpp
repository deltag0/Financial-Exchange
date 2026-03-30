#pragma once

#include <boost/interprocess/ipc/message_queue.hpp>
#include <string>
#include <memory>
#include <iostream>

namespace exchange::core {

class SharedQueue {
public:
    // Basic constructor for opening or creating a shared memory message queue.
    SharedQueue(const std::string& queue_name, size_t max_messages, size_t max_msg_size) {
        try {
            // Open an existing named message queue or create a new one
            mq_ = std::make_unique<boost::interprocess::message_queue>(
                boost::interprocess::open_or_create,
                queue_name.c_str(),
                max_messages,
                max_msg_size
            );
        } catch(boost::interprocess::interprocess_exception &ex) {
            std::cerr << "Failed opening/creating shared queue: " << ex.what() << std::endl;
            throw;
        }
    }

    // Static helper to clean up the named queue from the OS memory
    static void remove(const std::string& queue_name) {
        boost::interprocess::message_queue::remove(queue_name.c_str());
    }

    // Producer: Returns true if the message was sent successfully
    bool send(const void* data, size_t size, unsigned int priority = 0) {
        return mq_->try_send(data, size, priority);
    }

    // Consumer: Returns true if a message was received
    bool receive(void* buffer, size_t buffer_size, size_t& bytes_received, unsigned int& priority) {
        return mq_->try_receive(buffer, buffer_size, bytes_received, priority);
    }

    // Blocking receive that waits for a message
    void receive_blocking(void* buffer, size_t buffer_size, size_t& bytes_received, unsigned int& priority) {
        mq_->receive(buffer, buffer_size, bytes_received, priority);
    }

private:
    std::unique_ptr<boost::interprocess::message_queue> mq_;
};

} // namespace exchange::core
