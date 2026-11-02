#include "gateway/frame.hpp"
#include <algorithm>
#include <limits>
#include <stdexcept>

namespace gateway {
std::string encode_frame(const std::string& body) {
    if (body.empty() || body.size() > std::numeric_limits<std::uint32_t>::max())
        throw std::length_error("invalid frame body size");
    const auto n = static_cast<std::uint32_t>(body.size());
    std::string wire(4, '\0');
    for (unsigned i = 0; i < 4; ++i) wire[i] = static_cast<char>((n >> (24 - 8 * i)) & 0xff);
    wire += body;
    return wire;
}
FrameDecoder::FrameDecoder(std::size_t max_body, std::size_t receive_capacity)
    : max_body_(max_body), receive_capacity_(receive_capacity) {}
bool FrameDecoder::feed(std::span<const char> bytes, const Consumer& consume) {
    if (!error_.empty()) return false;
    while (!bytes.empty()) {
        if (header_used_ < 4) {
            header_[header_used_++] = static_cast<unsigned char>(bytes.front());
            bytes = bytes.subspan(1);
            if (header_used_ != 4) continue;
            std::uint32_t size = 0;
            for (const auto b : header_) size = (size << 8) | b;
            // Validate before reserve/append; even 0xffffffff never triggers allocation.
            if (size == 0) error_ = "empty_frame";
            else if (size > max_body_) error_ = "frame_too_large";
            else if (size > receive_capacity_) error_ = "receive_capacity";
            if (!error_.empty()) return false;
            expected_ = size;
            body_.reserve(expected_);
        }
        const auto take = std::min(expected_ - body_.size(), bytes.size());
        body_.append(bytes.data(), take);
        bytes = bytes.subspan(take);
        if (body_.size() == expected_) {
            const bool more = consume(body_);
            body_.clear();
            header_used_ = 0;
            expected_ = 0;
            if (!more) return false;
        }
    }
    return true;
}
}
