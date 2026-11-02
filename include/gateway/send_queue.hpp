#pragma once
#include <deque>
#include <memory>
#include <string>

namespace gateway {
// Strand-confined. Front includes the active write in both budgets.
class SendQueue {
public:
    SendQueue(std::size_t messages, std::size_t bytes) : max_messages_(messages), max_bytes_(bytes) {}
    bool push(std::shared_ptr<const std::string> frame) {
        if (frames_.size() >= max_messages_ || frame->size() > max_bytes_ - bytes_) return false;
        bytes_ += frame->size();
        frames_.push_back(std::move(frame));
        return true;
    }
    bool empty() const { return frames_.empty(); }
    auto front() const { return frames_.front(); }
    void pop() { bytes_ -= frames_.front()->size(); frames_.pop_front(); }
    void clear() { frames_.clear(); bytes_ = 0; }
    std::size_t bytes() const { return bytes_; }
private:
    std::size_t max_messages_, max_bytes_, bytes_ = 0;
    std::deque<std::shared_ptr<const std::string>> frames_;
};
}
