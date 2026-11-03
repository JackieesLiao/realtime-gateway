#pragma once

#include "gateway/frame.hpp"
#include "gateway/resource_budget.hpp"

#include <array>
#include <functional>
#include <span>

namespace gateway {

enum class FrameDisposition {
    Accepted, // 消费者已经处理完，或持有后续处理需要的数据副本。
    Paused,   // 尚未接管消息体，保留它等待下一次 process。
    Stopped   // 不再处理当前连接的后续帧。
};

enum class InputStatus {
    NeedRead, // 旧输入已消费且没有完整帧等待接管，允许发起下一次读。
    Paused,   // 完整帧待接管，或合法包头等待预算，不能覆盖输入。
    Stopped,  // 调用方已经终止处理。
    Error     // 解析错误，调用方应关闭连接。
};

// M2.1：Session 持有的输入状态，独立于 socket 以便确定性测试暂停和恢复。
// 所有调用都应在连接 strand 内；异步读期间不能处理或覆盖这块存储。
class BufferedInput {
public:
    using Consumer = std::function<FrameDisposition(const std::string&)>;

    BufferedInput(std::size_t max_body_bytes, std::size_t receive_body_capacity);

    // 只有旧输入全部消费且没有待处理完整帧时，才允许复用固定读缓冲。
    std::span<char> writable_buffer();
    void commit_read(std::size_t bytes_received);
    bool can_read() const;

    // Consumer 只借用消息体；返回 Accepted 前必须处理完或自行取得数据副本。
    // Paused 保留完整帧及原始尾部；再次 process 就是恢复入口。
    // consumed 在消息体存储销毁后调用，才能准确归还接收凭证。
    InputStatus process(const Consumer& consume_body,
        const FrameDecoder::Admission& admit = {},
        const std::function<void()>& consumed = {});
    void stop();

    std::size_t unread_bytes() const;
    bool has_frame() const;
    const std::string& error() const;

private:
    FrameDecoder decoder_;
    std::array<char, read_buffer_bytes> read_buffer_{};

    // 未消费区间是 [input_begin_, input_end_)，暂停时两个偏移都保持不变。
    std::size_t input_begin_ = 0;
    std::size_t input_end_ = 0;
    bool stopped_ = false;
};

} // namespace gateway
