#include "gateway/buffered_input.hpp"

#include <stdexcept>

namespace gateway {

BufferedInput::BufferedInput(std::size_t max_body_bytes, std::size_t receive_body_capacity)
    : decoder_(max_body_bytes, receive_body_capacity) {
}

std::span<char> BufferedInput::writable_buffer() {
    if (!can_read()) {
        throw std::logic_error("cannot overwrite unconsumed input");
    }

    return read_buffer_;
}

void BufferedInput::commit_read(std::size_t bytes_received) {
    if (!can_read()) {
        throw std::logic_error("cannot replace unconsumed input");
    }
    if (bytes_received > read_buffer_.size()) {
        throw std::length_error("read exceeds fixed input buffer");
    }

    input_begin_ = 0;
    input_end_ = bytes_received;
}

bool BufferedInput::can_read() const {
    return !stopped_ && decoder_.error().empty() && !decoder_.has_frame() &&
           !decoder_.awaiting_capacity() && input_begin_ == input_end_;
}

InputStatus BufferedInput::process(const Consumer& consume_body,
    const FrameDecoder::Admission& admit,
    const std::function<void()>& consumed) {
    if (stopped_) {
        return InputStatus::Stopped;
    }
    if (!decoder_.error().empty()) {
        return InputStatus::Error;
    }

    while (true) {
        // 恢复时先重试已经收齐的帧；不重新 feed，避免重复解析或丢失消息。
        if (decoder_.has_frame()) {
            const auto disposition = consume_body(decoder_.body());
            // 消费者可能同步触发关闭；终止优先于它返回的暂停或接管结果。
            if (disposition == FrameDisposition::Stopped || stopped_) {
                stopped_ = true;
                return InputStatus::Stopped;
            }
            if (disposition == FrameDisposition::Paused) {
                return InputStatus::Paused;
            }

            // 消费者确认接管后才释放当前帧，随后继续处理原始尾部。
            decoder_.take_body();
            if (consumed) {
                consumed();
            }
            continue;
        }

        if (input_begin_ == input_end_ && !decoder_.awaiting_capacity()) {
            input_begin_ = 0;
            input_end_ = 0;
            return InputStatus::NeedRead;
        }
        // read_buffer_.data() 指向缓冲区的第 0 个字节,加上 input_begin_，就是从第几个字节开始：
        const std::span<const char> remaining_bytes(
            read_buffer_.data() + input_begin_, input_end_ - input_begin_);
        const auto result = decoder_.feed(remaining_bytes, admit);
        input_begin_ += result.consumed_bytes;

        if (stopped_) {
            return InputStatus::Stopped;
        }
        if (result.status == DecodeStatus::AwaitingCapacity) {
            return InputStatus::Paused;
        }
        if (result.status == DecodeStatus::Error) {
            return InputStatus::Error;
        }

        // FrameReady 下一轮先交给消费者；NeedMore 已消费全部输入，会走 NeedRead。
        // 两种路径都会推进状态，不对消费量为 0 的完整帧反复调用 feed。
    }
}

void BufferedInput::stop() {
    // 在途读仍可能引用 read_buffer_，此处只标记终止，存储随 Session 一起销毁。
    stopped_ = true;
}

std::size_t BufferedInput::unread_bytes() const {
    return input_end_ - input_begin_;
}

bool BufferedInput::has_frame() const {
    return decoder_.has_frame();
}

const std::string& BufferedInput::error() const {
    return decoder_.error();
}

} // namespace gateway
