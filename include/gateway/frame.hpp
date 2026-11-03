#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace gateway {

// 协议格式：4 字节消息体长度（大端序）+ 消息体。此函数用于发送前打包。
std::string encode_frame(const std::string& body);

enum class DecodeStatus {
    NeedMore,         // 本次输入已消费，继续等待当前帧或下一帧的字节。
    FrameReady,       // 完整消息体保留到 take_body，不继续消费后续输入。
    AwaitingCapacity, // 包头已合法，但尚未取得完整流水线的空间。
    Error             // 终止状态，后续 feed 不再消费输入。
};

struct DecodeResult {
    // 只计算本次输入消费的字节，不包括之前调用已经保存的半包。
    std::size_t consumed_bytes;
    DecodeStatus status;
};

// M2.1：每次最多产出一帧；完整帧留在解析器中，直到调用方明确取走。
// 不读取 socket，也不调用业务回调。调用方必须串行访问同一个解析器。
class FrameDecoder {
public:
    FrameDecoder(std::size_t max_body_bytes, std::size_t receive_body_capacity);

    using Admission = std::function<bool(std::size_t)>;

    // 空 Admission 用于独立解析测试；Session 总是提供预算准入回调。
    DecodeResult feed(std::span<const char> remaining_bytes, const Admission& admit = {});
    bool has_frame() const;
    bool awaiting_capacity() const;

    // body() 只借用完整帧，引用有效期截止到 take_body() 或解析器销毁。
    // 两个接口在没有完整帧时都抛出 logic_error，避免误处理半包。
    const std::string& body() const;
    std::string take_body();

    const std::string& error() const {
        return error_;
    }

    std::size_t buffered_body_bytes() const {
        return body_.size();
    }

    std::size_t allocated_body_capacity() const {
        return body_.capacity();
    }

private:
    std::size_t max_body_bytes_;        // 协议允许的最大消息体长度。
    std::size_t receive_body_capacity_; // 当前连接允许接收的最大消息体容量。
    std::array<unsigned char, 4> header_{};
    std::size_t header_bytes_received_ = 0; // 当前已经收到几个包头字节。
    bool body_admitted_ = false;
    std::size_t expected_body_bytes_ = 0; // 从完整包头解析出的消息体长度。
    std::string body_;                    // 当前已经收到的消息体字节。
    std::string error_;
};

} // namespace gateway
