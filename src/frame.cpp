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

bool FrameDecoder::feed(std::span<const char> remaining_bytes, const Consumer& consume_body) {
    if (!error_.empty()) {
        return false;
    }

    while (!remaining_bytes.empty()) {
        if (header_bytes_received_ < header_.size()) {
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

            // Validate both limits before reserving memory, even for a header of 0xffffffff.
            if (body_length == 0) {
                error_ = "empty_frame";
            } else if (body_length > max_body_bytes_) {
                error_ = "frame_too_large";
            } else if (body_length > receive_body_capacity_) {
                error_ = "receive_capacity";
            }

            if (!error_.empty()) {
                return false;
            }

            expected_body_bytes_ = body_length;
            body_.reserve(expected_body_bytes_);
        }

        const auto missing_body_bytes = expected_body_bytes_ - body_.size();
        const auto bytes_to_copy = std::min(missing_body_bytes, remaining_bytes.size());
        body_.append(remaining_bytes.data(), bytes_to_copy);
        remaining_bytes = remaining_bytes.subspan(bytes_to_copy);

        if (body_.size() == expected_body_bytes_) {
            // The consumer may close the connection. Never process the next frame after that.
            const bool continue_decoding = consume_body(body_);
            body_.clear();
            header_bytes_received_ = 0;
            expected_body_bytes_ = 0;

            if (!continue_decoding) {
                return false;
            }
        }
    }

    return true;
}

} // namespace gateway
