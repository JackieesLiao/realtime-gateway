#include "gateway/frame.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gateway {

std::string encode_frame(const std::string& body) {
    if (body.empty() || body.size() > std::numeric_limits<std::uint32_t>::max()) {
        throw std::length_error("invalid frame body size");
    }

    const auto body_length = static_cast<std::uint32_t>(body.size());
    // 创建一个包含 4 个零字节的字符串，先为包头留位置：frame：00 00 00 00。
    std::string frame(4, '\0');

    for (unsigned byte_index = 0; byte_index < 4; ++byte_index) {
        const unsigned bit_shift = 24 - 8 * byte_index;
        frame[byte_index] = static_cast<char>((body_length >> bit_shift) & 0xff);
    }

    frame += body;
    return frame;
}

FrameDecoder::FrameDecoder(std::size_t max_body_bytes, std::size_t receive_body_capacity)
    : max_body_bytes_(max_body_bytes), receive_body_capacity_(receive_body_capacity) {
}

DecodeResult FrameDecoder::feed(std::span<const char> remaining_bytes, const Admission& admit) {
    if (!error_.empty()) {
        return {0, DecodeStatus::Error};
    }

    // 尚未取走的帧不能被新输入覆盖，也不能重复消费后面的字节。
    if (has_frame()) {
        return {0, DecodeStatus::FrameReady};
    }

    const auto input_size = remaining_bytes.size();
    while (true) {
        // 先处理包头还没收齐的情况，进度保存在成员中供下次 feed 继续。
        if (header_bytes_received_ < header_.size()) {
            if (remaining_bytes.empty()) {
                return {input_size, DecodeStatus::NeedMore};
            }
            header_[header_bytes_received_] = static_cast<unsigned char>(remaining_bytes.front());
            ++header_bytes_received_;
            remaining_bytes = remaining_bytes.subspan(1);

            if (header_bytes_received_ < header_.size()) {
                continue;
            }

            std::uint32_t body_length = 0;
            for (const auto header_byte : header_) {
                body_length = (body_length << 8) | header_byte;
            }

            // 必须先校验再分配；0xffffffff 这样的声明不会触发消息体分配。
            if (body_length == 0) {
                error_ = "empty_frame";
            } else if (body_length > max_body_bytes_) {
                error_ = "frame_too_large";
            } else if (body_length > receive_body_capacity_) {
                error_ = "receive_capacity";
            }

            if (!error_.empty()) {
                return {input_size - remaining_bytes.size(), DecodeStatus::Error};
            }

            expected_body_bytes_ = body_length;
        }

        // 即使包头恰好耗尽本次输入，也要先准入，不能直接启动下一次读。
        if (!body_admitted_) {
            if (admit && !admit(expected_body_bytes_)) {
                return {input_size - remaining_bytes.size(), DecodeStatus::AwaitingCapacity};
            }
            body_.reserve(expected_body_bytes_);
            body_admitted_ = true;
        }
        if (remaining_bytes.empty()) {
            return {input_size, DecodeStatus::NeedMore};
        }

        const auto missing_body_bytes = expected_body_bytes_ - body_.size();
        const auto bytes_to_copy = std::min(missing_body_bytes, remaining_bytes.size());
        body_.append(remaining_bytes.data(), bytes_to_copy);
        remaining_bytes = remaining_bytes.subspan(bytes_to_copy);

        if (has_frame()) {
            // 不清空消息体、不处理下一帧，消费量告诉 Session 从哪里恢复。
            return {input_size - remaining_bytes.size(), DecodeStatus::FrameReady};
        }
    }
}

bool FrameDecoder::awaiting_capacity() const {
    return error_.empty() && expected_body_bytes_ != 0 && !body_admitted_;
}

bool FrameDecoder::has_started_frame() const {
    return header_bytes_received_ != 0;
}

bool FrameDecoder::has_frame() const {
    return error_.empty() && expected_body_bytes_ != 0 && body_.size() == expected_body_bytes_;
}

const std::string& FrameDecoder::body() const {
    if (!has_frame()) {
        throw std::logic_error("no complete frame available");
    }

    return body_;
}

std::string FrameDecoder::take_body() {
    if (!has_frame()) {
        throw std::logic_error("cannot take an incomplete frame");
    }

    // swap 转移消息体的存储所有权；后续解析不会修改已经交出的消息体。
    std::string completed_body;
    completed_body.swap(body_);
    header_bytes_received_ = 0;
    expected_body_bytes_ = 0;
    body_admitted_ = false;
    return completed_body;
}

} // namespace gateway
