#include "gateway/resource_budget.hpp"
#include "gateway/send_queue.hpp"

#include <stdexcept>

#include <deque>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

using namespace gateway;

namespace {

ResourceBudgetLimits small_limits() {
    ResourceBudgetLimits limits;
    limits.connections = 4;
    limits.receive_body_bytes = 1024;
    return limits;
}

void collect_wakeups(
    ResourceBudget& budget, const ConnectionBudget& connection, std::deque<WakeupTicket>& wakeups) {
    budget.set_wakeup(connection.account, [&](WakeupTicket ticket) {
        wakeups.push_back(std::move(ticket));
    });
}

} // namespace

TEST(ResourceBudget, HeaderAdmissionReservesEveryStageAtomically) {
    ResourceBudget budget(small_limits());
    auto connection = budget.open(1);
    ASSERT_TRUE(connection);
    auto admitted = budget.try_frame(connection->account, 100);
    ASSERT_EQ(admitted.status, BudgetStatus::Acquired);
    const auto snapshot = budget.snapshot();
    EXPECT_EQ(snapshot.used[static_cast<std::size_t>(Resource::Read)].bytes, read_buffer_bytes);
    EXPECT_EQ(snapshot.used[static_cast<std::size_t>(Resource::Receive)].bytes, 100U);
    EXPECT_EQ(snapshot.used[static_cast<std::size_t>(Resource::Business)].bytes,
        100U + request_reserve_padding + max_ack_frame_bytes);
    EXPECT_EQ(snapshot.used[static_cast<std::size_t>(Resource::Send)].bytes, max_ack_frame_bytes);
    EXPECT_EQ(snapshot.total.messages, 3U);

    budget.close(connection->account);
    admitted = {};
    connection.reset();
    EXPECT_EQ(budget.snapshot().total.bytes, 0U);
}

TEST(ResourceBudget, PermanentFailureDoesNotPartiallyReserveOrWait) {
    auto limits = small_limits();
    limits.local_send.bytes = max_ack_frame_bytes - 1;
    ResourceBudget budget(limits);
    auto connection = budget.open(1);
    ASSERT_TRUE(connection);
    auto denied = budget.try_frame(connection->account, 100);
    EXPECT_EQ(denied.status, BudgetStatus::Impossible);
    EXPECT_EQ(denied.failure, BudgetFailure::Send);
    EXPECT_EQ(budget.snapshot().total.messages, 0U);
    EXPECT_EQ(budget.snapshot().budget_waiters, 0U);
    EXPECT_EQ(budget.snapshot().total.bytes, read_buffer_bytes);
}

TEST(ResourceBudget, OldEligibleWaiterCannotBeOvertakenByPreviousOwner) {
    auto limits = small_limits();
    limits.total.messages = 3;
    ResourceBudget budget(limits);
    auto first = budget.open(1);
    auto second = budget.open(2);
    std::deque<WakeupTicket> wakeups;
    collect_wakeups(budget, *first, wakeups);
    collect_wakeups(budget, *second, wakeups);
    auto held = budget.try_frame(first->account, 100);
    ASSERT_EQ(held.status, BudgetStatus::Acquired);
    EXPECT_EQ(budget.try_frame(second->account, 100).status, BudgetStatus::Waiting);
    EXPECT_TRUE(wakeups.empty());

    held = {};
    ASSERT_EQ(wakeups.size(), 1U);
    EXPECT_EQ(budget.try_frame(first->account, 100).status, BudgetStatus::Waiting);
    auto admitted = budget.try_frame(second->account, 100);
    EXPECT_EQ(admitted.status, BudgetStatus::Acquired);
    EXPECT_EQ(budget.snapshot().total.messages, 3U);
    EXPECT_LE(budget.snapshot().pending_wakeups, 2U);

    budget.close(first->account);
    budget.close(second->account);
    wakeups.clear();
}

TEST(ResourceBudget, WatermarksWaitUntilBothCountAndBytesReachLow) {
    auto limits = small_limits();
    limits.local_send = {4, 1024};
    limits.high_percent = 75;
    limits.low_percent = 25;
    ResourceBudget budget(limits);
    auto connection = budget.open(1);
    std::deque<WakeupTicket> wakeups;
    collect_wakeups(budget, *connection, wakeups);
    std::vector<ResourceLease> writes;
    for (int index = 0; index < 3; ++index) {
        auto frame = budget.try_frame(connection->account, 100);
        ASSERT_EQ(frame.status, BudgetStatus::Acquired);
        writes.push_back(std::move(frame.reservation.task.reply));
    }
    EXPECT_EQ(budget.try_frame(connection->account, 100).status, BudgetStatus::Waiting);
    EXPECT_TRUE(wakeups.empty());
    for (auto& write : writes) {
        write.shrink_bytes(6);
    }
    // 字节已低于低水位，但条数仍为三，不能仅凭字节下降就恢复。
    EXPECT_TRUE(wakeups.empty());
    writes[0].release();
    EXPECT_TRUE(wakeups.empty());
    // 仍有两条，低水位为一条；即使已有硬容量，也不能提前恢复。
    writes[1].release();
    ASSERT_EQ(wakeups.size(), 1U);
    auto frame = budget.try_frame(connection->account, 100);
    EXPECT_EQ(frame.status, BudgetStatus::Acquired);
    budget.close(connection->account);
    wakeups.clear();
}

TEST(ResourceBudget, ClosedConnectionRetainsReadCreditUntilLastOperationReleasesIt) {
    auto limits = small_limits();
    limits.connections = 1;
    ResourceBudget budget(limits);
    auto connection = budget.open(1);
    ASSERT_TRUE(connection);
    budget.close(connection->account);
    EXPECT_FALSE(budget.open(2));
    connection->read.release();
    auto next = budget.open(2);
    EXPECT_TRUE(next);
    EXPECT_EQ(budget.snapshot().used[0].bytes, read_buffer_bytes);
}

TEST(ResourceBudget, QueueEpochCatchesReleaseBetweenFailureAndWaitRegistration) {
    ResourceBudget budget(small_limits());
    auto connection = budget.open(1);
    std::deque<WakeupTicket> wakeups;
    collect_wakeups(budget, *connection, wakeups);
    const auto turn = budget.queue_turn(connection->account);
    ASSERT_TRUE(turn.allowed);
    budget.notify_task_capacity();
    budget.wait_queue(connection->account, turn.epoch);
    ASSERT_EQ(wakeups.size(), 1U);
    EXPECT_EQ(budget.snapshot().queue_waiters, 1U);
    // 连续释放不应再给同一连接投递第二个唤醒。
    budget.notify_task_capacity();
    budget.notify_task_capacity();
    EXPECT_EQ(wakeups.size(), 1U);
    budget.queue_accepted(connection->account);
    wakeups.clear();
    EXPECT_EQ(budget.snapshot().pending_wakeups, 0U);
}

TEST(ResourceBudget, FailedQueueRetryDoesNotSpinWithoutAnotherCapacityEvent) {
    ResourceBudget budget(small_limits());
    auto connection = budget.open(1);
    std::deque<WakeupTicket> wakeups;
    collect_wakeups(budget, *connection, wakeups);
    auto turn = budget.queue_turn(connection->account);
    budget.wait_queue(connection->account, turn.epoch);
    EXPECT_TRUE(wakeups.empty());
    budget.notify_task_capacity();
    ASSERT_EQ(wakeups.size(), 1U);
    {
        auto notification = std::move(wakeups.front());
        wakeups.pop_front();
        turn = budget.queue_turn(connection->account);
        ASSERT_TRUE(turn.allowed);
        budget.wait_queue(connection->account, turn.epoch);
    }
    EXPECT_TRUE(wakeups.empty());
    budget.close(connection->account);
}

TEST(ResourceBudget, ShrinkNeverGrowsAndAllMovesReturnBudgetOnce) {
    ResourceBudget budget(small_limits());
    auto connection = budget.open(1);
    auto frame = budget.try_frame(connection->account, 100);
    ASSERT_EQ(frame.status, BudgetStatus::Acquired);
    frame.reservation.task.reply.shrink_bytes(6);
    EXPECT_THROW(frame.reservation.task.reply.shrink_bytes(7), std::logic_error);
    ResourceLease moved(std::move(frame.reservation.task.reply));
    frame.reservation.task.reply.release();
    EXPECT_EQ(budget.snapshot().used[3].bytes, 6U);
    moved.release();
    moved.release();
    EXPECT_EQ(budget.snapshot().used[3].bytes, 0U);
}

TEST(ResourceBudget, ClosingWaitingAccountClearsRegistrationAndPendingTokenStillFinishes) {
    ResourceBudget budget(small_limits());
    auto connection = budget.open(1);
    std::deque<WakeupTicket> wakeups;
    collect_wakeups(budget, *connection, wakeups);
    const auto epoch = budget.queue_turn(connection->account).epoch;
    budget.wait_queue(connection->account, epoch);
    budget.notify_task_capacity();
    ASSERT_EQ(wakeups.size(), 1U);
    budget.close(connection->account);
    EXPECT_EQ(budget.snapshot().queue_waiters, 0U);
    wakeups.clear();
    connection.reset();
    EXPECT_EQ(budget.snapshot().pending_wakeups, 0U);
    EXPECT_EQ(budget.snapshot().total.bytes, 0U);
}

TEST(ResourceBudget, InvalidWatermarksAndReadHeadroomAreRejected) {
    auto limits = small_limits();
    limits.low_percent = limits.high_percent;
    EXPECT_THROW(ResourceBudget budget(limits), std::invalid_argument);
    limits = small_limits();
    limits.total.bytes = limits.connections * read_buffer_bytes;
    EXPECT_THROW(ResourceBudget budget(limits), std::invalid_argument);
}

TEST(ResourceBudget, CombinedByteLimitCannotBeBypassedByLargeStageLimits) {
    auto limits = small_limits();
    limits.total.bytes = limits.connections * read_buffer_bytes + 200 + request_reserve_padding +
                         2 * max_ack_frame_bytes;
    ResourceBudget budget(limits);
    auto first = budget.open(1);
    auto second = budget.open(2);
    std::deque<WakeupTicket> wakeups;
    collect_wakeups(budget, *second, wakeups);
    auto held = budget.try_frame(first->account, 100);
    ASSERT_EQ(held.status, BudgetStatus::Acquired);
    const auto before = budget.snapshot().total.bytes;
    EXPECT_EQ(budget.try_frame(second->account, 100).status, BudgetStatus::Waiting);
    EXPECT_EQ(budget.snapshot().total.bytes, before);
    held = {};
    ASSERT_EQ(wakeups.size(), 1U);
    auto admitted = budget.try_frame(second->account, 100);
    EXPECT_EQ(admitted.status, BudgetStatus::Acquired);
    EXPECT_LE(budget.snapshot().peak_total.bytes, limits.total.bytes);
    budget.close(first->account);
    budget.close(second->account);
    wakeups.clear();
}

TEST(ResourceBudget, ReceiveByteHighWaterResumesWhenHeldBodyIsConsumed) {
    auto limits = small_limits();
    limits.receive.bytes = 100;
    ResourceBudget budget(limits);
    auto first = budget.open(1);
    auto second = budget.open(2);
    std::deque<WakeupTicket> wakeups;
    collect_wakeups(budget, *second, wakeups);
    auto held = budget.try_frame(first->account, 100);
    ASSERT_EQ(held.status, BudgetStatus::Acquired);
    EXPECT_EQ(budget.try_frame(second->account, 1).status, BudgetStatus::Waiting);
    held.reservation.receive.release();
    ASSERT_EQ(wakeups.size(), 1U);
    auto admitted = budget.try_frame(second->account, 1);
    EXPECT_EQ(admitted.status, BudgetStatus::Acquired);
    EXPECT_LE(budget.snapshot().peak[1].bytes, limits.receive.bytes);
    budget.close(first->account);
    budget.close(second->account);
    wakeups.clear();
}

TEST(ResourceBudget, SlowWriteClearDoesNotReturnInFlightBudgetEarly) {
    auto limits = small_limits();
    limits.local_send = {1, max_ack_frame_bytes};
    ResourceBudget budget(limits);
    auto connection = budget.open(1);
    std::deque<WakeupTicket> wakeups;
    collect_wakeups(budget, *connection, wakeups);
    auto admitted = budget.try_frame(connection->account, 100);
    ASSERT_EQ(admitted.status, BudgetStatus::Acquired);
    admitted.reservation.receive.release();
    admitted.reservation.task.business.release();
    admitted.reservation.task.reply.shrink_bytes(6);
    auto frame =
        std::make_shared<const OutgoingFrame>("123456", std::move(admitted.reservation.task.reply));
    SendQueue queue(1, max_ack_frame_bytes);
    ASSERT_TRUE(queue.push(frame));
    auto active_write = queue.front();
    frame.reset();
    queue.clear();
    EXPECT_EQ(budget.snapshot().used[3].messages, 1U);
    EXPECT_EQ(budget.snapshot().used[3].bytes, 6U);
    EXPECT_EQ(budget.try_frame(connection->account, 100).status, BudgetStatus::Waiting);
    EXPECT_TRUE(wakeups.empty());

    active_write.reset();
    ASSERT_EQ(wakeups.size(), 1U);
    EXPECT_EQ(budget.snapshot().used[3].messages, 0U);
    budget.close(connection->account);
    wakeups.clear();
}

TEST(ResourceBudget, OversizedBodyArithmeticIsRejectedBeforeReservation) {
    ResourceBudget budget(small_limits());
    auto connection = budget.open(1);
    auto denied = budget.try_frame(connection->account, std::numeric_limits<std::size_t>::max());
    EXPECT_EQ(denied.status, BudgetStatus::Impossible);
    EXPECT_EQ(denied.failure, BudgetFailure::Receive);
    EXPECT_EQ(budget.snapshot().total.messages, 0U);
}

TEST(ResourceBudget, ThrowingWakeupReturnsPendingTicketWithoutThrowingFromRelease) {
    auto limits = small_limits();
    limits.total.messages = 3;
    ResourceBudget budget(limits);
    auto first = budget.open(1);
    auto second = budget.open(2);
    int attempts = 0;
    budget.set_wakeup(second->account, [&](WakeupTicket) {
        ++attempts;
        throw std::runtime_error("controlled notification failure");
    });
    auto held = budget.try_frame(first->account, 100);
    ASSERT_EQ(held.status, BudgetStatus::Acquired);
    EXPECT_EQ(budget.try_frame(second->account, 100).status, BudgetStatus::Waiting);
    EXPECT_NO_THROW(held = {});
    EXPECT_EQ(attempts, 1);
    EXPECT_EQ(budget.snapshot().pending_wakeups, 0U);
    auto admitted = budget.try_frame(second->account, 100);
    EXPECT_EQ(admitted.status, BudgetStatus::Acquired);
    budget.close(first->account);
    budget.close(second->account);
}

TEST(ResourceBudget, ByteLatchDoesNotResumeMerelyBecauseMessageCountIsLow) {
    auto limits = small_limits();
    limits.local_send = {100, 512};
    limits.high_percent = 75;
    limits.low_percent = 25;
    ResourceBudget budget(limits);
    auto connection = budget.open(1);
    std::deque<WakeupTicket> wakeups;
    collect_wakeups(budget, *connection, wakeups);
    std::vector<ResourceLease> writes;
    for (int index = 0; index < 2; ++index) {
        auto admitted = budget.try_frame(connection->account, 100);
        ASSERT_EQ(admitted.status, BudgetStatus::Acquired);
        writes.push_back(std::move(admitted.reservation.task.reply));
    }
    EXPECT_EQ(budget.try_frame(connection->account, 100).status, BudgetStatus::Waiting);
    writes[0].shrink_bytes(100);
    writes[1].shrink_bytes(100);
    EXPECT_TRUE(wakeups.empty());
    // 两条远低于条数低水位 25，但 200 字节仍高于字节低水位 128。
    writes[0].shrink_bytes(28);
    ASSERT_EQ(wakeups.size(), 1U);
    auto admitted = budget.try_frame(connection->account, 100);
    EXPECT_EQ(admitted.status, BudgetStatus::Acquired);
    budget.close(connection->account);
    wakeups.clear();
}
