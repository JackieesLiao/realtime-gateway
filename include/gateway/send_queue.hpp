#pragma once

#include "gateway/resource_budget.hpp"

#include <cstddef>
#include <deque>
#include <memory>
#include <string>
#include <utility>

namespace gateway {

// 在途回调和发送队列共享同一对象；最后一个持有者释放后才归还发送预算。
struct OutgoingFrame {
    OutgoingFrame(std::string wire, ResourceLease reservation = {})
        : budget(std::move(reservation)), bytes(std::move(wire)) {
    }

    ResourceLease budget;
    std::string bytes;
};

// Access only on the connection strand. Both budgets include the active write.
class SendQueue {
public:
    SendQueue(std::size_t max_messages, std::size_t max_bytes)
        : max_messages_(max_messages), max_bytes_(max_bytes) {
    }

    bool push(std::shared_ptr<const OutgoingFrame> frame) {
        const bool message_limit_reached = frames_.size() >= max_messages_;
        const bool frame_exceeds_byte_budget = frame->bytes.size() > max_bytes_ - bytes_;
        if (message_limit_reached || frame_exceeds_byte_budget) {
            return false;
        }

        bytes_ += frame->bytes.size();
        frames_.push_back(std::move(frame));
        return true;
    }

    bool empty() const {
        return frames_.empty();
    }

    std::shared_ptr<const OutgoingFrame> front() const {
        return frames_.front();
    }

    void pop() {
        bytes_ -= frames_.front()->bytes.size();
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
    std::deque<std::shared_ptr<const OutgoingFrame>> frames_;
};

} // namespace gateway
