#include "gateway/connection_policy.hpp"

#include <limits>
#include <stdexcept>

#include <gtest/gtest.h>

using namespace gateway;
using namespace std::chrono_literals;

namespace {

const PolicyTime start{};

ConnectionLimits untimed() {
    ConnectionLimits limits;
    limits.rate.messages_per_second = 0;
    limits.rate.bytes_per_second = 0;
    limits.timeout = {0ms, 0ms, 0ms, 0ms};
    return limits;
}

} // namespace

TEST(TokenBucket, FractionalCreditsUseExactNanosecondCeiling) {
    TokenBucket bucket(3, 1, start);
    bucket.consume(1, start);
    EXPECT_EQ(bucket.delay(1, start), 333333334ns);
    EXPECT_EQ(bucket.delay(1, start + 333333333ns), 1ns);
    EXPECT_EQ(bucket.delay(1, start + 333333334ns), 0ns);
    bucket.consume(1, start + 333333334ns);
    EXPECT_THROW(bucket.consume(1, start + 333333334ns), std::logic_error);
}

TEST(TokenBucket, ByteCostAndLongIdleRefillAreBounded) {
    TokenBucket bucket(100, 200, start);
    bucket.consume(180, start);
    EXPECT_EQ(bucket.delay(50, start), 300ms);
    EXPECT_FALSE(bucket.can_ever_fit(201));
    EXPECT_THROW(bucket.delay(201, start), std::invalid_argument);
    EXPECT_EQ(bucket.delay(200, start + std::chrono::hours(24)), 0ns);
    bucket.consume(200, start + std::chrono::hours(24));
    EXPECT_EQ(bucket.delay(200, start + std::chrono::hours(24)), 2s);
}

TEST(TokenBucket, DisabledLimitDoesNotMultiplyHugeCost) {
    TokenBucket bucket(0, 1, start);
    const auto huge = std::numeric_limits<std::size_t>::max();
    EXPECT_TRUE(bucket.can_ever_fit(huge));
    EXPECT_EQ(bucket.delay(huge, start), 0ns);
    EXPECT_NO_THROW(bucket.consume(huge, start));
}

TEST(TokenBucket, RejectsInvalidLimitsAndBackwardsTime) {
    EXPECT_THROW(TokenBucket(1, 0, start), std::invalid_argument);
    EXPECT_THROW(TokenBucket(1000000001, 1, start), std::invalid_argument);
    EXPECT_THROW(TokenBucket(1, 1073741825, start), std::invalid_argument);
    TokenBucket bucket(1, 1, start);
    EXPECT_THROW(bucket.delay(1, start - 1ns), std::logic_error);
}

TEST(ConnectionPolicy, BothRateBudgetsMustBeAvailableAndChecksDoNotCharge) {
    auto limits = untimed();
    limits.rate = {100, 1, 10, 10};
    ConnectionPolicy policy(limits, start);
    EXPECT_EQ(policy.check_rate(10, start).retry_at, start);
    EXPECT_EQ(policy.check_rate(10, start).retry_at, start);
    policy.consume_rate(10, start);
    EXPECT_EQ(policy.check_rate(10, start + 10ms).retry_at, start + 1s);
    EXPECT_THROW(policy.consume_rate(10, start + 10ms), std::logic_error);
    EXPECT_EQ(policy.check_rate(10, start + 1s).retry_at, start + 1s);
    EXPECT_FALSE(policy.check_rate(11, start + 1s).possible);
}

TEST(ConnectionPolicy, HeaderWaitUsesTimerAndPreservesOriginalWaitDeadline) {
    auto limits = untimed();
    limits.timeout.wait = 100ms;
    ConnectionPolicy policy(limits, start);
    policy.wait_for_rate(start + 80ms, start);
    EXPECT_EQ(policy.next_deadline(start), start + 80ms);
    EXPECT_FALSE(policy.take_rate_wakeup(start + 79ms));
    EXPECT_TRUE(policy.take_rate_wakeup(start + 80ms));
    EXPECT_FALSE(policy.take_rate_wakeup(start + 80ms));
    policy.wait_for(WaitReason::Resource, start + 80ms);
    policy.wait_for(WaitReason::Resource, start + 99ms);
    EXPECT_EQ(policy.next_deadline(start + 99ms), start + 100ms);
    EXPECT_EQ(policy.expired(start + 100ms), TimeoutReason::ResourceWait);
    policy.finish_wait();
    EXPECT_EQ(policy.expired(start + 101ms), TimeoutReason::None);
}

TEST(ConnectionPolicy, RateWaitTimeoutDoesNotNeedTokenAvailability) {
    auto limits = untimed();
    limits.timeout.wait = 50ms;
    ConnectionPolicy policy(limits, start);
    policy.wait_for_rate(start + 1s, start);
    policy.pause_peer(true, start);
    EXPECT_EQ(policy.next_deadline(start), start + 50ms);
    EXPECT_EQ(policy.expired(start + 50ms), TimeoutReason::RateWait);
}

TEST(ConnectionPolicy, FirstByteStartsFrameDeadlineAndDrippingDoesNotExtendIt) {
    auto limits = untimed();
    limits.timeout.frame = 500ms;
    ConnectionPolicy policy(limits, start);
    policy.frame_started(policy.peer_time(start + 100ms));
    policy.frame_started(policy.peer_time(start + 400ms));
    EXPECT_EQ(policy.expired(start + 599ms), TimeoutReason::None);
    EXPECT_EQ(policy.expired(start + 600ms), TimeoutReason::Frame);
    policy.frame_completed();
    EXPECT_EQ(policy.expired(start + 700ms), TimeoutReason::None);
}

TEST(ConnectionPolicy, ServerPauseIsExcludedFromPartialFrameTimeOnly) {
    auto limits = untimed();
    limits.timeout.frame = 500ms;
    limits.timeout.idle = 10s;
    ConnectionPolicy policy(limits, start);
    const auto arrived = policy.peer_time(start + 100ms);
    policy.pause_peer(true, start + 200ms);
    policy.pause_peer(true, start + 900ms);
    policy.frame_started(arrived);
    EXPECT_EQ(policy.expired(start + 2s), TimeoutReason::None);
    policy.pause_peer(false, start + 2200ms);
    EXPECT_EQ(policy.next_deadline(start + 2200ms), start + 2600ms);
    EXPECT_EQ(policy.expired(start + 2599ms), TimeoutReason::None);
    EXPECT_EQ(policy.expired(start + 2600ms), TimeoutReason::Frame);
}

TEST(ConnectionPolicy, InvalidFramesAndPauseDoNotRefreshWallClockIdleDeadline) {
    auto limits = untimed();
    limits.timeout.idle = 1s;
    ConnectionPolicy policy(limits, start);
    policy.frame_started(policy.peer_time(start + 500ms));
    policy.frame_completed();
    policy.pause_peer(true, start + 600ms);
    EXPECT_EQ(policy.next_deadline(start + 600ms), start + 1s);
    EXPECT_EQ(policy.expired(start + 1s), TimeoutReason::Idle);
}

TEST(ConnectionPolicy, ValidHeartbeatActivityPostponesIdleAndOldWakeupIsHarmless) {
    auto limits = untimed();
    limits.timeout.idle = 1s;
    ConnectionPolicy policy(limits, start);
    const auto old_deadline = policy.next_deadline(start);
    policy.valid_request(start + 800ms);
    EXPECT_EQ(policy.expired(*old_deadline), TimeoutReason::None);
    EXPECT_EQ(policy.next_deadline(*old_deadline), start + 1800ms);
    EXPECT_EQ(policy.expired(start + 1800ms), TimeoutReason::Idle);
}

TEST(ConnectionPolicy, WriteDeadlineContinuesDuringPauseAndEndsOnCompletion) {
    auto limits = untimed();
    limits.timeout.write = 100ms;
    ConnectionPolicy policy(limits, start);
    policy.write_started(start + 20ms);
    policy.pause_peer(true, start + 30ms);
    EXPECT_EQ(policy.expired(start + 120ms), TimeoutReason::Write);
    policy.write_finished();
    EXPECT_EQ(policy.expired(start + 121ms), TimeoutReason::None);
    policy.write_started(start + 150ms);
    EXPECT_EQ(policy.expired(start + 200ms), TimeoutReason::None);
    EXPECT_EQ(policy.next_deadline(start + 200ms), start + 250ms);
}

TEST(ConnectionPolicy, AllDisabledDeadlinesNeedNoTimer) {
    ConnectionPolicy policy(untimed(), start);
    policy.frame_started(0ns);
    policy.wait_for(WaitReason::Resource, start);
    policy.write_started(start);
    EXPECT_FALSE(policy.next_deadline(start + 10s));
    EXPECT_EQ(policy.expired(start + 10s), TimeoutReason::None);
}

TEST(ConnectionPolicy, InvalidTimeoutConfigurationFailsBeforeNetworking) {
    auto limits = untimed();
    limits.timeout.frame = -1ms;
    EXPECT_THROW(ConnectionPolicy(limits, start), std::invalid_argument);
    limits.timeout.frame = std::chrono::hours(2);
    EXPECT_THROW(ConnectionPolicy(limits, start), std::invalid_argument);
}

TEST(ConnectionPolicy, SimultaneousDeadlinesHaveStableWriteFirstPriority) {
    auto limits = untimed();
    limits.timeout = {100ms, 100ms, 100ms, 100ms};
    ConnectionPolicy policy(limits, start);
    policy.frame_started(0ns);
    policy.wait_for(WaitReason::Resource, start);
    policy.write_started(start);
    EXPECT_EQ(policy.expired(start + 100ms), TimeoutReason::Write);
}

TEST(ConnectionPolicy, DelayedParsingUsesOriginalCompleteFrameArrivalForIdle) {
    auto limits = untimed();
    limits.timeout.idle = 1s;
    ConnectionPolicy policy(limits, start);
    // 帧在 200ms 已到达，直到 900ms 才被下游允许解析；不能从 900ms 重新续期。
    policy.valid_request(start + 200ms);
    EXPECT_EQ(policy.next_deadline(start + 900ms), start + 1200ms);
    EXPECT_EQ(policy.expired(start + 1200ms), TimeoutReason::Idle);
}

TEST(TokenBucket, RefillCapsBeforeMultiplicationThatWouldWrapToZero) {
    constexpr std::uint64_t burst = 1073741824;
    TokenBucket bucket(1000000000, burst, start);
    bucket.consume(burst, start);
    // 2^55 纳秒乘 1e9 的低 64 位为零；直接相乘再截断会错误地认为没有补充。
    const auto long_idle = std::chrono::nanoseconds{std::int64_t{1} << 55};
    EXPECT_EQ(bucket.delay(burst, start + long_idle), 0ns);
}

TEST(ConnectionPolicy, FailedByteAdmissionPreservesAvailableMessageToken) {
    auto limits = untimed();
    limits.rate = {1, 2, 100, 100};
    ConnectionPolicy policy(limits, start);
    policy.consume_rate(100, start);
    // 尚有一个消息令牌，但字节已空；失败不能先扣掉那个消息令牌。
    EXPECT_THROW(policy.consume_rate(100, start), std::logic_error);
    EXPECT_EQ(policy.check_rate(1, start + 10ms).retry_at, start + 10ms);
}
