#include "gateway/bounded_task_queue.hpp"

#include <condition_variable>
#include <deque>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace gateway {

struct TaskQueueState {
    explicit TaskQueueState(TaskQueueLimits capacity) : limits(capacity) {
    }

    TaskQueueLimits limits;
    std::mutex mutex;
    std::condition_variable available;
    std::deque<std::unique_ptr<BusinessTask>> tasks;
    std::size_t queued_bytes = 0;
    std::size_t executing_messages = 0;
    std::size_t outstanding_messages = 0;
    std::size_t outstanding_bytes = 0;
    bool stopping = false;
};

TaskTicket::~TaskTicket() {
    release();
}

TaskTicket::TaskTicket(TaskTicket&& other) noexcept
    : state_(std::move(other.state_)),
      bytes_(std::exchange(other.bytes_, 0)),
      executing_(std::exchange(other.executing_, false)) {
}

TaskTicket& TaskTicket::operator=(TaskTicket&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        bytes_ = std::exchange(other.bytes_, 0);
        executing_ = std::exchange(other.executing_, false);
    }

    return *this;
}

void TaskTicket::finish_execution() {
    if (!state_ || !executing_) {
        return;
    }

    std::lock_guard lock(state_->mutex);
    --state_->executing_messages;
    executing_ = false;
}

void TaskTicket::release() {
    auto state = std::move(state_);
    if (!state) {
        return;
    }

    std::lock_guard lock(state->mutex);
    if (executing_) {
        --state->executing_messages;
    }
    --state->outstanding_messages;
    state->outstanding_bytes -= bytes_;
    executing_ = false;
    bytes_ = 0;
}

BoundedTaskQueue::BoundedTaskQueue(TaskQueueLimits limits)
    : state_(std::make_shared<TaskQueueState>(limits)) {
    if (limits.queued_messages == 0 || limits.queued_bytes == 0 ||
        limits.outstanding_messages == 0 || limits.outstanding_bytes == 0) {
        throw std::invalid_argument("task queue limits must be positive");
    }
}

BoundedTaskQueue::~BoundedTaskQueue() {
    stop();
}

SubmitStatus BoundedTaskQueue::try_push(
    std::unique_ptr<realtime::Request>& request, BusinessCompletion completion) {
    if (!request || !completion) {
        throw std::invalid_argument("request and completion are required");
    }

    const auto request_bytes = request->ByteSizeLong();
    auto task = std::make_unique<BusinessTask>();
    {
        std::lock_guard lock(state_->mutex);
        if (state_->stopping) {
            return SubmitStatus::Stopping;
        }

        const auto& limits = state_->limits;
        if (state_->tasks.size() >= limits.queued_messages ||
            request_bytes > limits.queued_bytes - state_->queued_bytes ||
            state_->outstanding_messages >= limits.outstanding_messages ||
            request_bytes > limits.outstanding_bytes - state_->outstanding_bytes) {
            return SubmitStatus::Full;
        }

        // 先完成可能分配内存的入队；后续只做 noexcept 所有权移动和计数更新。
        // 因此 Full/Stopping 或入队分配失败都不会提前夺走调用方的 Request。
        state_->tasks.push_back(std::move(task));
        auto& stored = *state_->tasks.back();
        stored.request = std::move(request);
        stored.completion = std::move(completion);
        stored.ticket.state_ = state_;
        stored.ticket.bytes_ = request_bytes;
        state_->queued_bytes += request_bytes;
        ++state_->outstanding_messages;
        state_->outstanding_bytes += request_bytes;
    }

    state_->available.notify_one();
    return SubmitStatus::Accepted;
}

std::unique_ptr<BusinessTask> BoundedTaskQueue::wait_pop() {
    std::unique_lock lock(state_->mutex);
    state_->available.wait(lock, [this] {
        return state_->stopping || !state_->tasks.empty();
    });
    if (state_->stopping) {
        return nullptr;
    }

    auto task = std::move(state_->tasks.front());
    state_->tasks.pop_front();
    state_->queued_bytes -= task->ticket.bytes_;
    ++state_->executing_messages;
    task->ticket.executing_ = true;
    return task;
}

void BoundedTaskQueue::stop() {
    std::deque<std::unique_ptr<BusinessTask>> discarded;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        discarded.swap(state_->tasks);
        state_->queued_bytes = 0;
    }

    state_->available.notify_all();
    // 必须在锁外销毁：TaskTicket 析构也需要这把锁。
    // 同时打破 State -> 排队 Task -> Ticket -> State 的临时所有权环。
}

TaskQueueSnapshot BoundedTaskQueue::snapshot() const {
    std::lock_guard lock(state_->mutex);
    return {state_->tasks.size(),
        state_->queued_bytes,
        state_->executing_messages,
        state_->outstanding_messages,
        state_->outstanding_bytes,
        state_->stopping};
}

} // namespace gateway
