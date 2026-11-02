#include "gateway/buffered_input.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace gateway;

namespace {

// 直接写入生产代码使用的固定缓冲，避免测试依赖 TCP 实际分包结果。
void stage_input(BufferedInput& input, const std::string& bytes) {
    const auto buffer = input.writable_buffer();
    ASSERT_LE(bytes.size(), buffer.size());
    std::copy(bytes.begin(), bytes.end(), buffer.begin());
    input.commit_read(bytes.size());
}

} // namespace

TEST(BufferedInput, PauseAtMiddleFrameThenDrainExistingTail) {
    BufferedInput input(64, 64);
    const auto third = encode_frame("charlie");
    const auto tail = encode_frame("tail");
    const auto wire = encode_frame("alpha") + encode_frame("bravo") + third + tail.substr(0, 6);
    stage_input(input, wire);

    bool allow_bravo = false;
    std::vector<std::string> accepted;
    const auto consumer = [&](const std::string& body) {
        if (body == "bravo" && !allow_bravo) {
            return FrameDisposition::Paused;
        }

        accepted.push_back(body);
        return FrameDisposition::Accepted;
    };

    ASSERT_EQ(input.process(consumer), InputStatus::Paused);
    EXPECT_EQ(accepted, (std::vector<std::string>{"alpha"}));
    EXPECT_TRUE(input.has_frame());
    EXPECT_EQ(input.unread_bytes(), third.size() + 6);
    EXPECT_FALSE(input.can_read());

    // 重试仍不获准时，完整帧和输入偏移都保持不变，不能覆盖旧缓冲。
    EXPECT_EQ(input.process(consumer), InputStatus::Paused);
    EXPECT_EQ(input.unread_bytes(), third.size() + 6);
    EXPECT_EQ(accepted.size(), 1U);
    EXPECT_THROW(input.writable_buffer(), std::logic_error);
    EXPECT_THROW(input.commit_read(0), std::logic_error);

    allow_bravo = true;
    ASSERT_EQ(input.process(consumer), InputStatus::NeedRead);
    EXPECT_EQ(accepted, (std::vector<std::string>{"alpha", "bravo", "charlie"}));
    EXPECT_EQ(input.unread_bytes(), 0U);
    EXPECT_TRUE(input.can_read());

    stage_input(input, tail.substr(6));
    EXPECT_EQ(input.process(consumer), InputStatus::NeedRead);
    EXPECT_EQ(accepted, (std::vector<std::string>{"alpha", "bravo", "charlie", "tail"}));
}

TEST(BufferedInput, PausedFrameAtInputEndStillPreventsRead) {
    BufferedInput input(64, 64);
    stage_input(input, encode_frame("held"));

    EXPECT_EQ(input.process([](const auto&) {
        return FrameDisposition::Paused;
    }),
        InputStatus::Paused);
    EXPECT_EQ(input.unread_bytes(), 0U);
    EXPECT_TRUE(input.has_frame());
    EXPECT_FALSE(input.can_read());
    EXPECT_THROW(input.writable_buffer(), std::logic_error);
    EXPECT_THROW(input.commit_read(0), std::logic_error);

    int accepted = 0;
    EXPECT_EQ(input.process([&](const auto& body) {
        EXPECT_EQ(body, "held");
        ++accepted;
        return FrameDisposition::Accepted;
    }),
        InputStatus::NeedRead);
    EXPECT_EQ(accepted, 1);
    EXPECT_TRUE(input.can_read());
}

TEST(BufferedInput, PartialBodySurvivesReadBufferReuseAndPause) {
    BufferedInput input(64, 64);
    const auto wire = encode_frame("hello");
    stage_input(input, wire.substr(0, 6));
    EXPECT_EQ(input.process([](const auto&) {
        ADD_FAILURE() << "partial body reached consumer";
        return FrameDisposition::Accepted;
    }),
        InputStatus::NeedRead);

    stage_input(input, wire.substr(6) + encode_frame("next"));
    EXPECT_EQ(input.process([](const auto& body) {
        EXPECT_EQ(body, "hello");
        return FrameDisposition::Paused;
    }),
        InputStatus::Paused);

    std::vector<std::string> accepted;
    EXPECT_EQ(input.process([&](const auto& body) {
        accepted.push_back(body);
        return FrameDisposition::Accepted;
    }),
        InputStatus::NeedRead);
    EXPECT_EQ(accepted, (std::vector<std::string>{"hello", "next"}));
}

TEST(BufferedInput, ByteByByteReadsPreserveProgress) {
    BufferedInput input(64, 64);
    const auto wire = encode_frame("hello") + encode_frame("world");
    std::vector<std::string> accepted;
    for (const auto byte : wire) {
        ASSERT_TRUE(input.can_read());
        stage_input(input, std::string(1, byte));
        EXPECT_EQ(input.process([&](const auto& body) {
            accepted.push_back(body);
            return FrameDisposition::Accepted;
        }),
            InputStatus::NeedRead);
    }

    EXPECT_EQ(accepted, (std::vector<std::string>{"hello", "world"}));
}

TEST(BufferedInput, LargeBodySpansSeveralFixedReadBuffers) {
    BufferedInput input(10000, 10000);
    const auto body = std::string(9000, static_cast<char>(0x80));
    const auto wire = encode_frame(body) + encode_frame("next");
    std::vector<std::string> accepted;
    std::size_t offset = 0;
    while (offset < wire.size()) {
        const auto capacity = input.writable_buffer().size();
        const auto size = std::min(capacity, wire.size() - offset);
        stage_input(input, wire.substr(offset, size));
        offset += size;
        ASSERT_EQ(input.process([&](const auto& complete_body) {
            accepted.push_back(complete_body);
            return FrameDisposition::Accepted;
        }),
            InputStatus::NeedRead);
    }

    EXPECT_EQ(accepted, (std::vector<std::string>{body, "next"}));
}

TEST(BufferedInput, StopWhilePausedIsTerminal) {
    BufferedInput input(64, 64);
    stage_input(input, encode_frame("held") + encode_frame("tail"));
    ASSERT_EQ(input.process([](const auto&) {
        return FrameDisposition::Paused;
    }),
        InputStatus::Paused);

    input.stop();
    input.stop();
    EXPECT_EQ(input.process([](const auto&) {
        ADD_FAILURE() << "consumer called after stop";
        return FrameDisposition::Accepted;
    }),
        InputStatus::Stopped);
    EXPECT_FALSE(input.can_read());
    EXPECT_THROW(input.writable_buffer(), std::logic_error);
    EXPECT_THROW(input.commit_read(0), std::logic_error);
}

TEST(BufferedInput, ConsumerStopDoesNotProcessFollowingFrames) {
    BufferedInput input(64, 64);
    const auto tail = encode_frame("never");
    stage_input(input, encode_frame("first") + encode_frame("stop") + tail);
    std::vector<std::string> visited;
    EXPECT_EQ(input.process([&](const auto& body) {
        visited.push_back(body);
        if (body == "stop") {
            return FrameDisposition::Stopped;
        }
        return FrameDisposition::Accepted;
    }),
        InputStatus::Stopped);
    EXPECT_EQ(visited, (std::vector<std::string>{"first", "stop"}));
    EXPECT_EQ(input.unread_bytes(), tail.size());
    EXPECT_FALSE(input.can_read());
}

TEST(BufferedInput, ErrorDoesNotConsumeFollowingFrameOrCallConsumer) {
    BufferedInput input(64, 64);
    const auto next = encode_frame("never");
    stage_input(input, std::string(4, static_cast<char>(0xff)) + next);
    const auto consumer = [](const auto&) {
        ADD_FAILURE() << "invalid frame reached consumer";
        return FrameDisposition::Accepted;
    };

    EXPECT_EQ(input.process(consumer), InputStatus::Error);
    EXPECT_EQ(input.error(), "frame_too_large");
    EXPECT_EQ(input.unread_bytes(), next.size());
    EXPECT_EQ(input.process(consumer), InputStatus::Error);
    EXPECT_FALSE(input.can_read());
    EXPECT_THROW(input.writable_buffer(), std::logic_error);
}

TEST(BufferedInput, RejectsOverwritingUnreadBytesAndOversizedCommit) {
    BufferedInput input(64, 64);
    const auto capacity = input.writable_buffer().size();
    EXPECT_THROW(input.commit_read(capacity + 1), std::length_error);

    stage_input(input, encode_frame("retained"));
    EXPECT_THROW(input.writable_buffer(), std::logic_error);
    EXPECT_THROW(input.commit_read(0), std::logic_error);
}

TEST(BufferedInput, ConsumerTriggeredStopTakesPriorityOverItsReturnValue) {
    for (const auto disposition : {
             FrameDisposition::Accepted,
             FrameDisposition::Paused,
             FrameDisposition::Stopped,
         }) {
        BufferedInput input(64, 64);
        stage_input(input, encode_frame("first") + encode_frame("never"));
        EXPECT_EQ(input.process([&](const auto&) {
            input.stop();
            return disposition;
        }),
            InputStatus::Stopped);

        EXPECT_FALSE(input.can_read());
        EXPECT_EQ(input.process([](const auto&) {
            ADD_FAILURE() << "consumer called after synchronous close";
            return FrameDisposition::Accepted;
        }),
            InputStatus::Stopped);
    }
}
