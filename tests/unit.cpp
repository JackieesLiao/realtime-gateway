#include "gateway/business.hpp"
#include "gateway/config.hpp"
#include "gateway/frame.hpp"
#include "gateway/send_queue.hpp"

#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

using namespace gateway;

TEST(Frame, EveryTwoPartSplit) {
    const auto wire = encode_frame("telemetry-body");
    for (std::size_t split = 0; split <= wire.size(); ++split) {
        FrameDecoder decoder(64, 64);
        const auto first = decoder.feed(std::span(wire).first(split));
        EXPECT_EQ(first.consumed_bytes, split);

        if (split == wire.size()) {
            ASSERT_EQ(first.status, DecodeStatus::FrameReady);
            EXPECT_EQ(decoder.take_body(), "telemetry-body");
            const auto empty = decoder.feed({});
            EXPECT_EQ(empty.status, DecodeStatus::NeedMore);
            EXPECT_EQ(empty.consumed_bytes, 0U);
        } else {
            ASSERT_EQ(first.status, DecodeStatus::NeedMore);
            const auto second = decoder.feed(std::span(wire).subspan(split));
            EXPECT_EQ(second.consumed_bytes, wire.size() - split);
            ASSERT_EQ(second.status, DecodeStatus::FrameReady);
            EXPECT_EQ(decoder.take_body(), "telemetry-body");
        }

        EXPECT_FALSE(decoder.has_frame());
    }
}

TEST(Frame, ByteByByte) {
    const auto wire = encode_frame("abcdef");
    FrameDecoder decoder(64, 64);
    for (std::size_t index = 0; index < wire.size(); ++index) {
        const auto result = decoder.feed(std::span(wire).subspan(index, 1));
        EXPECT_EQ(result.consumed_bytes, 1U);
        if (index + 1 == wire.size()) {
            EXPECT_EQ(result.status, DecodeStatus::FrameReady);
        } else {
            EXPECT_EQ(result.status, DecodeStatus::NeedMore);
            EXPECT_FALSE(decoder.has_frame());
        }
    }

    EXPECT_EQ(decoder.take_body(), "abcdef");
}

TEST(Frame, MultipleFramesAndIncompleteTail) {
    const auto first_wire = encode_frame("first");
    const auto second_wire = encode_frame("second");
    const auto tail = encode_frame("third");
    const auto wire = first_wire + second_wire + tail.substr(0, 6);
    FrameDecoder decoder(64, 64);

    // 即使一次传入多帧，消费量也严格停在当前帧边界。
    const auto first = decoder.feed(wire);
    ASSERT_EQ(first.status, DecodeStatus::FrameReady);
    EXPECT_EQ(first.consumed_bytes, first_wire.size());
    EXPECT_EQ(decoder.take_body(), "first");

    const auto second = decoder.feed(std::span(wire).subspan(first.consumed_bytes));
    ASSERT_EQ(second.status, DecodeStatus::FrameReady);
    EXPECT_EQ(second.consumed_bytes, second_wire.size());
    EXPECT_EQ(decoder.take_body(), "second");

    const auto consumed = first.consumed_bytes + second.consumed_bytes;
    const auto partial = decoder.feed(std::span(wire).subspan(consumed));
    EXPECT_EQ(partial.status, DecodeStatus::NeedMore);
    EXPECT_EQ(partial.consumed_bytes, 6U);
    EXPECT_EQ(decoder.buffered_body_bytes(), 2U);

    const auto completed = decoder.feed(std::span(tail).subspan(6));
    ASSERT_EQ(completed.status, DecodeStatus::FrameReady);
    EXPECT_EQ(completed.consumed_bytes, tail.size() - 6);
    EXPECT_EQ(decoder.take_body(), "third");
}

TEST(Frame, RejectsOversizeBeforeAllocatingBody) {
    FrameDecoder decoder(1048576, 1048576);
    const auto capacity = decoder.allocated_body_capacity();
    const auto wire = std::string(4, static_cast<char>(0xff)) + "unconsumed";
    const auto result = decoder.feed(wire);

    EXPECT_EQ(result.status, DecodeStatus::Error);
    EXPECT_EQ(result.consumed_bytes, 4U);
    EXPECT_EQ(decoder.error(), "frame_too_large");
    EXPECT_EQ(decoder.buffered_body_bytes(), 0U);
    EXPECT_EQ(decoder.allocated_body_capacity(), capacity);
    EXPECT_FALSE(decoder.has_frame());

    const auto retry = decoder.feed("anything");
    EXPECT_EQ(retry.status, DecodeStatus::Error);
    EXPECT_EQ(retry.consumed_bytes, 0U);
}

TEST(Frame, RejectsReceiveCapacityBeforeAllocatingBody) {
    FrameDecoder decoder(64, 8);
    const auto capacity = decoder.allocated_body_capacity();
    const auto wire = encode_frame(std::string(9, 'x'));
    const auto result = decoder.feed(wire);

    EXPECT_EQ(result.status, DecodeStatus::Error);
    EXPECT_EQ(result.consumed_bytes, 4U);
    EXPECT_EQ(decoder.error(), "receive_capacity");
    EXPECT_EQ(decoder.allocated_body_capacity(), capacity);
}

TEST(Frame, ExactReceiveAndFrameBoundary) {
    FrameDecoder decoder(8, 8);
    const auto wire = encode_frame("12345678");
    const auto result = decoder.feed(wire);

    ASSERT_EQ(result.status, DecodeStatus::FrameReady);
    EXPECT_EQ(result.consumed_bytes, wire.size());
    EXPECT_EQ(decoder.take_body(), "12345678");
}

TEST(Frame, EmptyFrameIsTerminalError) {
    FrameDecoder decoder(8, 8);
    const auto result = decoder.feed(std::string(4, '\0'));

    EXPECT_EQ(result.status, DecodeStatus::Error);
    EXPECT_EQ(result.consumed_bytes, 4U);
    EXPECT_EQ(decoder.error(), "empty_frame");
    EXPECT_THROW(decoder.body(), std::logic_error);
    EXPECT_THROW(decoder.take_body(), std::logic_error);
}

TEST(Frame, ReadyFrameRemainsUntilTaken) {
    FrameDecoder decoder(64, 64);
    const auto first = encode_frame("first");
    const auto second = encode_frame("second");
    ASSERT_EQ(decoder.feed(first).status, DecodeStatus::FrameReady);

    // 暂停时新输入和空输入都不能覆盖尚未取走的完整帧。
    for (const auto input : {std::span<const char>(second), std::span<const char>()}) {
        const auto result = decoder.feed(input);
        EXPECT_EQ(result.status, DecodeStatus::FrameReady);
        EXPECT_EQ(result.consumed_bytes, 0U);
        EXPECT_EQ(decoder.body(), "first");
    }

    const auto owned_body = decoder.take_body();
    EXPECT_FALSE(decoder.has_frame());
    EXPECT_EQ(decoder.buffered_body_bytes(), 0U);
    ASSERT_EQ(decoder.feed(second).status, DecodeStatus::FrameReady);
    EXPECT_EQ(decoder.take_body(), "second");
    EXPECT_EQ(owned_body, "first");
}

TEST(Frame, EmptyInputPreservesPartialHeader) {
    FrameDecoder decoder(64, 64);
    const auto empty = decoder.feed({});
    EXPECT_EQ(empty.status, DecodeStatus::NeedMore);
    EXPECT_EQ(empty.consumed_bytes, 0U);

    const auto wire = encode_frame("hello");
    const auto partial = decoder.feed(std::span(wire).first(2));
    EXPECT_EQ(partial.status, DecodeStatus::NeedMore);
    EXPECT_EQ(partial.consumed_bytes, 2U);
    EXPECT_EQ(decoder.feed({}).status, DecodeStatus::NeedMore);

    ASSERT_EQ(decoder.feed(std::span(wire).subspan(2)).status, DecodeStatus::FrameReady);
    EXPECT_EQ(decoder.take_body(), "hello");
}

TEST(Frame, TakingPartialBodyFailsWithoutDiscardingIt) {
    FrameDecoder decoder(64, 64);
    const auto wire = encode_frame("hello");
    ASSERT_EQ(decoder.feed(std::span(wire).first(6)).status, DecodeStatus::NeedMore);
    EXPECT_EQ(decoder.buffered_body_bytes(), 2U);
    EXPECT_THROW(decoder.body(), std::logic_error);
    EXPECT_THROW(decoder.take_body(), std::logic_error);
    EXPECT_EQ(decoder.buffered_body_bytes(), 2U);

    ASSERT_EQ(decoder.feed(std::span(wire).subspan(6)).status, DecodeStatus::FrameReady);
    EXPECT_EQ(decoder.take_body(), "hello");
}

TEST(SendQueue, CountIncludesInFlightAndBudgetReturns) {
    SendQueue queue(1, 100);
    auto frame = std::make_shared<const std::string>("1234");
    EXPECT_TRUE(queue.push(frame));

    auto active_write_buffer = queue.front();
    EXPECT_FALSE(queue.push(frame));
    EXPECT_EQ(queue.bytes(), 4U);

    queue.pop();
    EXPECT_TRUE(queue.push(frame));

    queue.clear();
    EXPECT_EQ(queue.bytes(), 0U);
    EXPECT_TRUE(queue.empty());
    EXPECT_EQ(*active_write_buffer, "1234");
}

TEST(SendQueue, ByteBoundaryAndOversizedSingleFrame) {
    SendQueue queue(3, 8);
    auto frame = std::make_shared<const std::string>("1234");
    EXPECT_TRUE(queue.push(frame));
    EXPECT_TRUE(queue.push(frame));
    EXPECT_FALSE(queue.push(std::make_shared<const std::string>("x")));

    queue.pop();
    EXPECT_TRUE(queue.push(frame));

    queue.clear();
    EXPECT_FALSE(queue.push(std::make_shared<const std::string>(9, 'x')));
    EXPECT_EQ(queue.bytes(), 0U);
}

static realtime::Request valid_request() {
    realtime::Request request;
    request.set_request_id(42);
    request.set_device_id("robot-1");
    request.set_message_type(realtime::TELEMETRY);
    request.set_payload("abc");
    return request;
}

TEST(Business, AckAfterCountersAndCorrelation) {
    Statistics stats;
    Router router(stats);
    auto ack = router.route(valid_request());
    EXPECT_EQ(ack.request_id(), 42U);
    EXPECT_EQ(ack.result(), realtime::OK);
    EXPECT_EQ(stats.processed.load(), 1U);
    EXPECT_EQ(stats.payload_bytes.load(), 3U);
}

TEST(Business, ValidationAndUnknownTypes) {
    Statistics stats;
    Router router(stats);
    auto request = valid_request();
    request.set_message_type(static_cast<realtime::MessageType>(999));
    EXPECT_EQ(router.route(request).result(), realtime::UNKNOWN_MESSAGE_TYPE);

    request.set_device_id(std::string(129, 'x'));
    EXPECT_EQ(router.route(request).result(), realtime::INVALID_REQUEST);

    request = valid_request();
    request.clear_request_id();
    EXPECT_EQ(router.route(request).result(), realtime::INVALID_REQUEST);

    request = valid_request();
    request.clear_payload();
    EXPECT_EQ(router.route(request).result(), realtime::INVALID_REQUEST);

    EXPECT_EQ(stats.rejected.load(), 4U);
    EXPECT_EQ(stats.processed.load(), 0U);
}

TEST(Protobuf, RejectsTruncatedLengthDelimitedField) {
    realtime::Request request;
    EXPECT_FALSE(request.ParseFromString(std::string("\x12\x05x", 3)));
}

TEST(Config, RejectsUnsafeSettingsAndTypos) {
    const auto path = std::filesystem::temp_directory_path() /
                      ("realtime-gateway-unit-config-" + std::to_string(getpid()) + ".json");
    const std::vector<std::string> invalid_configs{
        "{\"max_body_bytes\":-1}",
        "{\"io_threads\":0}",
        "{\"port\":65536}",
        "{\"max_send_bytes\":0}",
        "{\"typo\":1}",
        "{\"io_threads\":1.5}",
        "{\"business_threads\":0}",
        "{\"max_business_queue_bytes\":0}",
        "{\"max_business_outstanding_messages\":0}",
    };

    for (const auto& document : invalid_configs) {
        {
            std::ofstream file(path);
            file << document;
        }
        EXPECT_THROW(Config::load(path.string()), std::exception) << document;
    }

    {
        std::ofstream file(path);
        file << "{\"port\":0,\"max_receive_body_bytes\":8}";
    }

    EXPECT_EQ(Config::load(path.string()).max_receive_body_bytes, 8U);
    std::filesystem::remove(path);
}
