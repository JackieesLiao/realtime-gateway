#include "gateway/business.hpp"
#include "gateway/config.hpp"
#include "gateway/frame.hpp"
#include "gateway/send_queue.hpp"

#include <filesystem>
#include <fstream>
#include <vector>

#include <gtest/gtest.h>
#include <unistd.h>

using namespace gateway;

TEST(Frame, EveryTwoPartSplit) {
    const auto wire = encode_frame("telemetry-body");
    for (std::size_t split = 0; split <= wire.size(); ++split) {
        FrameDecoder decoder(64, 64);
        std::vector<std::string> bodies;

        const auto consume = [&](const auto& body) {
            bodies.push_back(body);
            return true;
        };
        EXPECT_TRUE(decoder.feed(std::span(wire).first(split), consume));
        EXPECT_TRUE(decoder.feed(std::span(wire).subspan(split), consume));
        EXPECT_EQ(bodies, std::vector<std::string>{"telemetry-body"}) << split;
    }
}

TEST(Frame, ByteByByte) {
    const auto wire = encode_frame("abcdef");
    FrameDecoder decoder(64, 64);
    int count = 0;
    for (const auto& byte : wire) {
        ASSERT_TRUE(decoder.feed({&byte, 1}, [&](const auto& body) {
            EXPECT_EQ(body, "abcdef");
            ++count;
            return true;
        }));
    }
    EXPECT_EQ(count, 1);
}

TEST(Frame, MultipleFramesAndIncompleteTail) {
    const auto tail = encode_frame("third");
    const auto wire = encode_frame("first") + encode_frame("second") + tail.substr(0, 6);
    FrameDecoder decoder(64, 64);
    std::vector<std::string> bodies;

    const auto consume = [&](const auto& body) {
        bodies.push_back(body);
        return true;
    };

    ASSERT_TRUE(decoder.feed(wire, consume));
    EXPECT_EQ(bodies, (std::vector<std::string>{"first", "second"}));
    EXPECT_EQ(decoder.buffered_body_bytes(), 2U);

    ASSERT_TRUE(decoder.feed(std::span(tail).subspan(6), consume));
    EXPECT_EQ(bodies.back(), "third");
    EXPECT_EQ(bodies.size(), 3U);
}

TEST(Frame, RejectsOversizeBeforeAllocatingBody) {
    FrameDecoder decoder(1048576, 1048576);
    const auto capacity = decoder.allocated_body_capacity();
    const std::string header(4, static_cast<char>(0xff));
    EXPECT_FALSE(decoder.feed(header, [](const auto&) {
        ADD_FAILURE();
        return true;
    }));
    EXPECT_EQ(decoder.error(), "frame_too_large");
    EXPECT_EQ(decoder.buffered_body_bytes(), 0U);
    EXPECT_EQ(decoder.allocated_body_capacity(), capacity);
    EXPECT_FALSE(decoder.feed("anything", [](const auto&) {
        return true;
    }));
}

TEST(Frame, RejectsReceiveCapacityBeforeAllocatingBody) {
    FrameDecoder decoder(64, 8);
    const auto capacity = decoder.allocated_body_capacity();
    const auto wire = encode_frame(std::string(9, 'x'));
    EXPECT_FALSE(decoder.feed(wire, [](const auto&) {
        ADD_FAILURE();
        return true;
    }));
    EXPECT_EQ(decoder.error(), "receive_capacity");
    EXPECT_EQ(decoder.allocated_body_capacity(), capacity);
}

TEST(Frame, ExactReceiveAndFrameBoundary) {
    FrameDecoder decoder(8, 8);
    EXPECT_TRUE(decoder.feed(encode_frame("12345678"), [](const auto& body) {
        EXPECT_EQ(body.size(), 8U);
        return true;
    }));
}

TEST(Frame, EmptyFrameAndConsumerStop) {
    FrameDecoder empty(8, 8);
    EXPECT_FALSE(empty.feed(std::string(4, '\0'), [](const auto&) {
        ADD_FAILURE();
        return true;
    }));
    EXPECT_EQ(empty.error(), "empty_frame");

    FrameDecoder stopped(8, 8);
    int count = 0;
    EXPECT_FALSE(stopped.feed(encode_frame("a") + encode_frame("b"), [&](const auto&) {
        ++count;
        return false;
    }));
    EXPECT_EQ(count, 1);
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
    Dispatcher dispatcher(stats);
    auto ack = dispatcher.dispatch(valid_request());
    EXPECT_EQ(ack.request_id(), 42U);
    EXPECT_EQ(ack.result(), realtime::OK);
    EXPECT_EQ(stats.processed.load(), 1U);
    EXPECT_EQ(stats.payload_bytes.load(), 3U);
}

TEST(Business, ValidationAndUnknownTypes) {
    Statistics stats;
    Dispatcher dispatcher(stats);
    auto request = valid_request();
    request.set_message_type(static_cast<realtime::MessageType>(999));
    EXPECT_EQ(dispatcher.dispatch(request).result(), realtime::UNKNOWN_MESSAGE_TYPE);

    request.set_device_id(std::string(129, 'x'));
    EXPECT_EQ(dispatcher.dispatch(request).result(), realtime::INVALID_REQUEST);

    request = valid_request();
    request.clear_request_id();
    EXPECT_EQ(dispatcher.dispatch(request).result(), realtime::INVALID_REQUEST);

    request = valid_request();
    request.clear_payload();
    EXPECT_EQ(dispatcher.dispatch(request).result(), realtime::INVALID_REQUEST);

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
