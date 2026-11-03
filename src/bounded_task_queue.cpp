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
    bool queued_paused = false;

    void update_watermark() {
        const auto high_count = (limits.queued_messages * limits.high_percent + 99) / 100;
        const auto high_bytes = (limits.queued_bytes * limits.high_percent + 99) / 100;
        if (tasks.size() >= high_count || queued_bytes >= high_bytes) {
            queued_paused = true;
        } else if (tasks.size() <= limits.queued_messages * limits.low_percent / 100 &&
                   queued_bytes <= limits.queued_bytes * limits.low_percent / 100) {
            queued_paused = false;
        }
    }

    std::shared_ptr<const std::function<void()>> notifier;
};

TaskTicket::~TaskTicket() {
    release();
}

TaskTicket::TaskTicket(TaskTicket&& other) noexcept
    : state_(std::move(other.state_)),
      bytes_(std::exchange(other.bytes_, 0)),
      executing_(std::exchange(other.executing_, false)),
      resources_(std::move(other.resources_)) {
}

TaskTicket& TaskTicket::operator=(TaskTicket&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        bytes_ = std::exchange(other.bytes_, 0);
        executing_ = std::exchange(other.executing_, false);
        resources_ = std::move(other.resources_);
    }

    return *this;
}

void TaskTicket::finish_execution() {
    if (!state_ || !executing_) {
        return;
    }

    {
        std::lock_guard lock(state_->mutex);
        --state_->executing_messages;
        executing_ = false;
    }
    // Request 已销毁，只剩完成结果；统一业务预算可缩到 ACK 对象上界。
    if (resources_.business && resources_.business.bytes() > max_ack_frame_bytes) {
        resources_.business.shrink_bytes(max_ack_frame_bytes);
    }
}

ResourceLease TaskTicket::take_reply_budget() {
    return std::move(resources_.reply);
}

void TaskTicket::release() {
    auto state = std::move(state_);
    auto resources = std::move(resources_);
    if (!state) {
        return;
    }

    std::shared_ptr<const std::function<void()>> notifier;
    {
        std::lock_guard lock(state->mutex);
        if (executing_) {
            --state->executing_messages;
        }
        --state->outstanding_messages;
        state->outstanding_bytes -= bytes_;
        executing_ = false;
        bytes_ = 0;
        notifier = state->notifier;
    }
    // 两类预算都在队列锁外归还；通知只投递恢复，不执行网络发送。
    resources.business.release();
    resources.reply.release();
    if (notifier) {
        (*notifier)();
    }
}

BoundedTaskQueue::BoundedTaskQueue(TaskQueueLimits limits)
    : state_(std::make_shared<TaskQueueState>(limits)) {
    if (limits.queued_messages == 0 || limits.queued_bytes == 0 ||
        limits.outstanding_messages == 0 || limits.outstanding_bytes == 0 ||
        limits.low_percent >= limits.high_percent || limits.high_percent > 99) {
        throw std::invalid_argument("task queue limits must be positive");
    }
}

BoundedTaskQueue::~BoundedTaskQueue() {
    stop();
}

SubmitStatus BoundedTaskQueue::try_push(std::unique_ptr<realtime::Request>& request,
    BusinessCompletion completion,
    TaskResources* resources) {
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
        if (state_->queued_paused || state_->tasks.size() >= limits.queued_messages ||
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
        if (resources) {
            stored.ticket.resources_ = std::move(*resources);
        }
        state_->queued_bytes += request_bytes;
        ++state_->outstanding_messages;
        state_->outstanding_bytes += request_bytes;
        state_->update_watermark();
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
    state_->update_watermark();
    ++state_->executing_messages;
    task->ticket.executing_ = true;
    const auto notifier = state_->notifier;
    lock.unlock();
    if (notifier) {
        (*notifier)();
    }
    return task;
}

void BoundedTaskQueue::stop() {
    std::deque<std::unique_ptr<BusinessTask>> discarded;
    {
        std::lock_guard lock(state_->mutex);
        state_->stopping = true;
        discarded.swap(state_->tasks);
        state_->queued_bytes = 0;
        state_->queued_paused = false;
    }

    state_->available.notify_all();
    std::shared_ptr<const std::function<void()>> notifier;
    {
        std::lock_guard lock(state_->mutex);
        notifier = state_->notifier;
    }
    if (notifier) {
        (*notifier)();
    }
    // 必须在锁外销毁：TaskTicket 析构也需要这把锁。
    // 同时打破 State -> 排队 Task -> Ticket -> State 的临时所有权环。
}

void BoundedTaskQueue::set_capacity_notifier(std::function<void()> notifier) {
    std::lock_guard lock(state_->mutex);
    state_->notifier = std::make_shared<const std::function<void()>>(std::move(notifier));
}

TaskQueueSnapshot BoundedTaskQueue::snapshot() const {
    std::lock_guard lock(state_->mutex);
    return {state_->tasks.size(),
        state_->queued_bytes,
        state_->executing_messages,
        state_->outstanding_messages,
        state_->outstanding_bytes,
        state_->stopping,
        state_->queued_paused};
}

} // namespace gateway
