#pragma once

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <utility>

namespace gateway {

// Access only on the connection strand. Both budgets include the active write.
class SendQueue {
public:
    SendQueue(std::size_t max_messages, std::size_t max_bytes)
        : max_messages_(max_messages), max_bytes_(max_bytes) {
    }

    bool push(std::shared_ptr<const std::string> frame) {
        const bool message_limit_reached = frames_.size() >= max_messages_;
        const bool frame_exceeds_byte_budget = frame->size() > max_bytes_ - bytes_;
        if (message_limit_reached || frame_exceeds_byte_budget) {
            return false;
        }

        bytes_ += frame->size();
        frames_.push_back(std::move(frame));
        return true;
    }

    bool empty() const {
        return frames_.empty();
    }

    std::shared_ptr<const std::string> front() const {
        return frames_.front();
    }

    void pop() {
        bytes_ -= frames_.front()->size();
        frames_.pop_front();
    }

    void clear() {
        frames_.clear();
        bytes_ = 0;
    }

    std::size_t bytes() const {
        return bytes_;
    }

private:
    std::size_t max_messages_;
    std::size_t max_bytes_;
    std::size_t bytes_ = 0;
    std::deque<std::shared_ptr<const std::string>> frames_;
};

} // namespace gateway
