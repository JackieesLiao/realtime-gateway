#include "gateway/dispatcher.hpp"

#include <chrono>
#include <condition_variable>
#include <future>
#include <mutex>
#include <stdexcept>
#include <utility>

#include <boost/asio.hpp>
#include <gtest/gtest.h>

using namespace gateway;
using namespace std::chrono_literals;

namespace {

std::unique_ptr<realtime::Request> make_request(std::uint64_t id = 1) {
    auto request = std::make_unique<realtime::Request>();
    request->set_request_id(id);
    request->set_device_id("robot-1");
    request->set_message_type(realtime::TELEMETRY);
    request->set_payload("abc");
    return request;
}

void discard_result(BusinessResult) {
}

realtime::Ack ok_result(const realtime::Request&) {
    realtime::Ack ack;
    ack.set_result(realtime::OK);
    return ack;
}

// 通过 gate 控制实际执行时刻；TearDown 总会放行，断言失败也不会卡在 join。
class DispatcherTest : public testing::Test {
protected:
    void start_blocked(TaskQueueLimits limits = {}) {
        dispatcher_ = std::make_unique<Dispatcher>(1, limits, [this](const auto& request) {
            {
                std::unique_lock lock(mutex_);
                ++entered_;
                changed_.notify_all();
                changed_.wait(lock, [this] {
                    return open_;
                });
            }
            return ok_result(request);
        });
    }

    bool wait_entered(std::size_t count) {
        std::unique_lock lock(mutex_);
        return changed_.wait_for(lock, 3s, [this, count] {
            return entered_ >= count;
        });
    }

    void open_gate() {
        {
            std::lock_guard lock(mutex_);
            open_ = true;
        }
        changed_.notify_all();
    }

    void TearDown() override {
        open_gate();
        if (dispatcher_) {
            dispatcher_->stop();
            dispatcher_->join();
        }
    }

    std::mutex mutex_;
    std::condition_variable changed_;
    std::size_t entered_ = 0;
    bool open_ = false;
    std::unique_ptr<Dispatcher> dispatcher_;
};

} // namespace

TEST(TaskQueue, QueueCountBoundaryPreservesRejectedRequest) {
    TaskQueueLimits limits;
    limits.queued_messages = 2;
    limits.outstanding_messages = 3;
    BoundedTaskQueue queue(limits);
    auto first = make_request(1);
    auto second = make_request(2);
    auto third = make_request(3);
    EXPECT_EQ(queue.try_push(first, discard_result), SubmitStatus::Accepted);
    EXPECT_FALSE(first);
    EXPECT_EQ(queue.try_push(second, discard_result), SubmitStatus::Accepted);
    EXPECT_EQ(queue.try_push(third, discard_result), SubmitStatus::Full);
    ASSERT_TRUE(third);
    EXPECT_EQ(third->request_id(), 3U);

    auto running = queue.wait_pop();
    EXPECT_EQ(queue.snapshot().queued_messages, 1U);
    EXPECT_EQ(queue.snapshot().executing_messages, 1U);
    EXPECT_EQ(queue.try_push(third, discard_result), SubmitStatus::Accepted);
    running.reset();
    queue.stop();
    EXPECT_EQ(queue.snapshot().outstanding_messages, 0U);
}

TEST(TaskQueue, QueueByteLimitIncludesExactBoundary) {
    auto first = make_request(1);
    const auto bytes = first->ByteSizeLong();
    TaskQueueLimits limits;
    limits.queued_bytes = bytes;
    limits.outstanding_bytes = 2 * bytes;
    BoundedTaskQueue queue(limits);
    auto second = make_request(2);
    EXPECT_EQ(queue.try_push(first, discard_result), SubmitStatus::Accepted);
    EXPECT_EQ(queue.snapshot().queued_bytes, bytes);
    EXPECT_EQ(queue.try_push(second, discard_result), SubmitStatus::Full);
    ASSERT_TRUE(second);

    auto running = queue.wait_pop();
    EXPECT_EQ(queue.snapshot().queued_bytes, 0U);
    EXPECT_EQ(queue.snapshot().outstanding_bytes, bytes);
    EXPECT_EQ(queue.try_push(second, discard_result), SubmitStatus::Accepted);
    running.reset();
    queue.stop();
    EXPECT_EQ(queue.snapshot().outstanding_bytes, 0U);
}

TEST(TaskQueue, OutstandingCountIncludesCompletedButUnconsumedResult) {
    TaskQueueLimits limits;
    limits.outstanding_messages = 1;
    BoundedTaskQueue queue(limits);
    auto first = make_request(1);
    ASSERT_EQ(queue.try_push(first, discard_result), SubmitStatus::Accepted);
    auto task = queue.wait_pop();
    BusinessResult result;
    result.ticket = std::move(task->ticket);
    task.reset();
    result.ticket.finish_execution();

    EXPECT_EQ(queue.snapshot().queued_messages, 0U);
    EXPECT_EQ(queue.snapshot().executing_messages, 0U);
    EXPECT_EQ(queue.snapshot().outstanding_messages, 1U);
    auto second = make_request(2);
    EXPECT_EQ(queue.try_push(second, discard_result), SubmitStatus::Full);
    result.ticket.release();
    result.ticket.release();
    EXPECT_EQ(queue.try_push(second, discard_result), SubmitStatus::Accepted);
}

TEST(TaskQueue, OutstandingBytesCannotBeBypassedByPoppingQueue) {
    auto first = make_request(1);
    const auto bytes = first->ByteSizeLong();
    TaskQueueLimits limits;
    limits.outstanding_bytes = bytes;
    BoundedTaskQueue queue(limits);
    ASSERT_EQ(queue.try_push(first, discard_result), SubmitStatus::Accepted);
    auto task = queue.wait_pop();
    auto second = make_request(2);
    EXPECT_EQ(queue.try_push(second, discard_result), SubmitStatus::Full);
    EXPECT_EQ(queue.snapshot().outstanding_bytes, bytes);
    task.reset();
    EXPECT_EQ(queue.snapshot().outstanding_bytes, 0U);
    EXPECT_EQ(queue.try_push(second, discard_result), SubmitStatus::Accepted);
}

TEST(TaskQueue, StopDiscardsWaitingTasksButKeepsRunningBudget) {
    BoundedTaskQueue queue({});
    auto first = make_request(1);
    auto second = make_request(2);
    ASSERT_EQ(queue.try_push(first, discard_result), SubmitStatus::Accepted);
    ASSERT_EQ(queue.try_push(second, discard_result), SubmitStatus::Accepted);
    auto running = queue.wait_pop();
    queue.stop();
    queue.stop();
    EXPECT_EQ(queue.snapshot().queued_messages, 0U);
    EXPECT_EQ(queue.snapshot().outstanding_messages, 1U);
    EXPECT_EQ(queue.snapshot().executing_messages, 1U);
    EXPECT_FALSE(queue.wait_pop());

    auto rejected = make_request(3);
    EXPECT_EQ(queue.try_push(rejected, discard_result), SubmitStatus::Stopping);
    EXPECT_TRUE(rejected);
    running.reset();
    EXPECT_EQ(queue.snapshot().outstanding_messages, 0U);
}

TEST(TaskQueue, TicketCanOutliveQueueAndMoveWithoutDoubleRelease) {
    TaskTicket retained;
    {
        BoundedTaskQueue queue({});
        auto request = make_request();
        ASSERT_EQ(queue.try_push(request, discard_result), SubmitStatus::Accepted);
        auto task = queue.wait_pop();
        retained = std::move(task->ticket);
        task.reset();
        retained.finish_execution();
        EXPECT_EQ(queue.snapshot().outstanding_messages, 1U);
    }

    TaskTicket moved(std::move(retained));
    retained.release();
    moved.release();
    moved.release();
}

TEST(TaskQueue, RejectsZeroLimitsAndMissingInputs) {
    TaskQueueLimits limits;
    limits.queued_bytes = 0;
    EXPECT_THROW(BoundedTaskQueue queue(limits), std::invalid_argument);
    BoundedTaskQueue queue({});
    std::unique_ptr<realtime::Request> missing;
    EXPECT_THROW(queue.try_push(missing, discard_result), std::invalid_argument);
    auto request = make_request();
    EXPECT_THROW(queue.try_push(request, {}), std::invalid_argument);
    EXPECT_TRUE(request);
}

TEST_F(DispatcherTest, FullQueueDoesNotWaitForBlockedWorker) {
    TaskQueueLimits limits;
    limits.queued_messages = 1;
    start_blocked(limits);
    auto first = make_request(1);
    ASSERT_EQ(dispatcher_->try_submit(first, discard_result), SubmitStatus::Accepted);
    ASSERT_TRUE(wait_entered(1));
    auto second = make_request(2);
    ASSERT_EQ(dispatcher_->try_submit(second, discard_result), SubmitStatus::Accepted);
    auto third = make_request(3);
    EXPECT_EQ(dispatcher_->try_submit(third, discard_result), SubmitStatus::Full);
    EXPECT_TRUE(third);
    EXPECT_EQ(dispatcher_->snapshot().queued_messages, 1U);
    EXPECT_EQ(dispatcher_->snapshot().executing_messages, 1U);
}

TEST_F(DispatcherTest, StopCancelsQueuedTaskWithoutEarlyRunningRelease) {
    start_blocked();
    auto completed = std::make_shared<std::atomic<int>>(0);
    auto completion = [completed](BusinessResult) {
        completed->fetch_add(1);
    };
    auto first = make_request(1);
    auto second = make_request(2);
    ASSERT_EQ(dispatcher_->try_submit(first, completion), SubmitStatus::Accepted);
    ASSERT_TRUE(wait_entered(1));
    ASSERT_EQ(dispatcher_->try_submit(second, completion), SubmitStatus::Accepted);
    dispatcher_->stop();
    EXPECT_EQ(dispatcher_->snapshot().queued_messages, 0U);
    EXPECT_EQ(dispatcher_->snapshot().outstanding_messages, 1U);
    EXPECT_EQ(completed->load(), 0);

    open_gate();
    dispatcher_->join();
    EXPECT_EQ(completed->load(), 1);
    EXPECT_EQ(dispatcher_->snapshot().outstanding_messages, 0U);
}

TEST(Dispatcher, HandlerExceptionProducesCorrelatedErrorAndWorkerSurvives) {
    const auto caller_thread = std::this_thread::get_id();
    std::promise<std::thread::id> executing_thread;
    std::promise<BusinessResult> failed;
    auto failed_future = failed.get_future();
    std::promise<BusinessResult> succeeded;
    auto success_future = succeeded.get_future();
    Dispatcher dispatcher(1, {}, [&](const realtime::Request& request) {
        if (request.request_id() == 1) {
            executing_thread.set_value(std::this_thread::get_id());
            throw std::runtime_error("controlled business failure");
        }
        return ok_result(request);
    });
    auto first = make_request(1);
    ASSERT_EQ(dispatcher.try_submit(first,
                  [&](BusinessResult result) {
                      failed.set_value(std::move(result));
                  }),
        SubmitStatus::Accepted);
    ASSERT_EQ(failed_future.wait_for(3s), std::future_status::ready);
    auto failure = failed_future.get();
    EXPECT_EQ(failure.ack.request_id(), 1U);
    EXPECT_EQ(failure.ack.result(), realtime::INTERNAL_ERROR);
    EXPECT_EQ(failure.ack.error(), "business handler failed");
    EXPECT_NE(executing_thread.get_future().get(), caller_thread);
    failure.ticket.release();

    auto second = make_request(2);
    ASSERT_EQ(dispatcher.try_submit(second,
                  [&](BusinessResult result) {
                      succeeded.set_value(std::move(result));
                  }),
        SubmitStatus::Accepted);
    ASSERT_EQ(success_future.wait_for(3s), std::future_status::ready);
    auto success = success_future.get();
    EXPECT_EQ(success.ack.request_id(), 2U);
    EXPECT_EQ(success.ack.result(), realtime::OK);
    EXPECT_EQ(dispatcher.failed_tasks(), 1U);
}

TEST(Dispatcher, PendingIoCompletionKeepsOutstandingCapacityReserved) {
    boost::asio::io_context io;
    TaskQueueLimits limits;
    limits.outstanding_messages = 1;
    std::promise<void> posted;
    auto posted_future = posted.get_future();
    int consumed = 0;
    Dispatcher dispatcher(1, limits, ok_result);
    auto request = make_request(1);
    ASSERT_EQ(dispatcher.try_submit(request,
                  [&](BusinessResult result) {
                      // 在完成回调查询快照，顺便验证调用回调时没有持队列锁。
                      EXPECT_EQ(dispatcher.snapshot().executing_messages, 0U);
                      boost::asio::post(io, [&, result = std::move(result)] {
                          EXPECT_EQ(result.ack.request_id(), 1U);
                          ++consumed;
                      });
                      posted.set_value();
                  }),
        SubmitStatus::Accepted);
    ASSERT_EQ(posted_future.wait_for(3s), std::future_status::ready);
    EXPECT_EQ(dispatcher.snapshot().outstanding_messages, 1U);
    auto second = make_request(2);
    EXPECT_EQ(dispatcher.try_submit(second, discard_result), SubmitStatus::Full);
    EXPECT_EQ(consumed, 0);

    io.poll();
    EXPECT_EQ(consumed, 1);
    EXPECT_EQ(dispatcher.snapshot().outstanding_messages, 0U);
    EXPECT_EQ(dispatcher.try_submit(second, discard_result), SubmitStatus::Accepted);
}

TEST(Dispatcher, ThrowingCompletionDoesNotTerminateWorker) {
    std::promise<void> completed;
    auto future = completed.get_future();
    Dispatcher dispatcher(1, {}, ok_result);
    auto first = make_request(1);
    ASSERT_EQ(dispatcher.try_submit(first,
                  [](BusinessResult) {
                      throw std::runtime_error("controlled completion failure");
                  }),
        SubmitStatus::Accepted);
    auto second = make_request(2);
    ASSERT_EQ(dispatcher.try_submit(second,
                  [&](BusinessResult) {
                      completed.set_value();
                  }),
        SubmitStatus::Accepted);
    ASSERT_EQ(future.wait_for(3s), std::future_status::ready);
    dispatcher.stop();
    dispatcher.join();
    EXPECT_EQ(dispatcher.failed_completions(), 1U);
    EXPECT_EQ(dispatcher.snapshot().outstanding_messages, 0U);
}
