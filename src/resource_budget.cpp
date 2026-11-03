#include "gateway/resource_budget.hpp"

#include <algorithm>
#include <deque>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace gateway {
namespace {

std::size_t index(Resource resource) {
    return static_cast<std::size_t>(resource);
}

bool fits(ResourceUsage used, ResourceUsage extra, ResourceUsage limit) {
    return extra.messages <= limit.messages - used.messages &&
           extra.bytes <= limit.bytes - used.bytes;
}

void add(ResourceUsage& used, ResourceUsage extra) {
    used.messages += extra.messages;
    used.bytes += extra.bytes;
}

void subtract(ResourceUsage& used, ResourceUsage returned) {
    used.messages -= returned.messages;
    used.bytes -= returned.bytes;
}

ResourceUsage sum(const std::array<ResourceUsage, 4>& usage, bool include_read) {
    ResourceUsage result;
    for (std::size_t position = 0; position < usage.size(); ++position) {
        if (position != index(Resource::Read) || include_read) {
            add(result, usage[position]);
        }
    }
    return result;
}

void update_latch(
    bool& latched, ResourceUsage used, ResourceUsage limit, unsigned high, unsigned low) {
    const auto high_messages = (limit.messages * high + 99) / 100;
    const auto high_bytes = (limit.bytes * high + 99) / 100;
    if (used.messages >= high_messages || used.bytes >= high_bytes) {
        latched = true;
    } else if (used.messages <= limit.messages * low / 100 &&
               used.bytes <= limit.bytes * low / 100) {
        latched = false;
    }
}

enum class WaitKind { None, Budget, Queue };

} // namespace

struct BudgetAccount {
    std::uint64_t id = 0;
    std::array<ResourceUsage, 4> used{};
    std::array<bool, 4> latched{};
    std::shared_ptr<const ResourceBudget::Wakeup> wakeup;
    WaitKind waiting = WaitKind::None;
    std::size_t wanted_body = 0;
    std::uint64_t notified_epoch = 0;
    bool scheduled = false;
    bool closed = false;
};

struct BudgetNotification {
    std::shared_ptr<const ResourceBudget::Wakeup> callback;
    WakeupTicket ticket;
};

struct ResourceBudgetState : std::enable_shared_from_this<ResourceBudgetState> {
    explicit ResourceBudgetState(ResourceBudgetLimits capacity) : limits(capacity) {
        global[index(Resource::Read)] = {0, limits.connections * read_buffer_bytes};
        global[index(Resource::Receive)] = limits.receive;
        global[index(Resource::Business)] = limits.business;
        global[index(Resource::Send)] = limits.send;
        local[index(Resource::Read)] = {0, read_buffer_bytes};
        local[index(Resource::Receive)] = {1, limits.receive_body_bytes};
        local[index(Resource::Business)] = {1, limits.business.bytes};
        local[index(Resource::Send)] = limits.local_send;
        dynamic_limit = {limits.total.messages, limits.total.bytes - global[0].bytes};
    }

    BudgetFailure impossible(std::size_t body) const {
        const ResourceUsage rx{1, body};
        if (body == 0 || !fits({}, rx, local[index(Resource::Receive)]) ||
            !fits({}, rx, limits.receive)) {
            return BudgetFailure::Receive;
        }
        const ResourceUsage business{1, body + request_reserve_padding + max_ack_frame_bytes};
        const ResourceUsage reply{1, max_ack_frame_bytes};
        if (!fits({}, business, global[index(Resource::Business)])) {
            return BudgetFailure::Business;
        }
        if (!fits({}, reply, limits.local_send) || !fits({}, reply, limits.send)) {
            return BudgetFailure::Send;
        }
        ResourceUsage total = rx;
        add(total, business);
        add(total, reply);
        if (!fits({}, total, dynamic_limit)) {
            return BudgetFailure::Total;
        }
        return BudgetFailure::None;
    }

    bool available(const BudgetAccount& account, std::size_t body) const {
        if (account.closed) {
            return false;
        }
        // body=0 表示只申请下一次读的许可，不分配动态帧空间。
        // 若最小帧也永久不可能容纳，允许读包头后报告具体硬上限错误。
        if (body == 0 && impossible(1) != BudgetFailure::None) {
            return true;
        }
        const auto size = std::max<std::size_t>(body, 1);
        const std::array<ResourceUsage, 4> extra{{{},
            {1, size},
            {1, size + request_reserve_padding + max_ack_frame_bytes},
            {1, max_ack_frame_bytes}}};
        if (total_latched) {
            return false;
        }
        for (std::size_t position = 1; position < extra.size(); ++position) {
            if (account.latched[position] || latched[position] ||
                !fits(account.used[position], extra[position], local[position]) ||
                !fits(used[position], extra[position], global[position])) {
                return false;
            }
        }
        return fits(sum(used, false), sum(extra, false), dynamic_limit);
    }

    void update() {
        for (std::size_t position = 1; position < used.size(); ++position) {
            update_latch(latched[position],
                used[position],
                global[position],
                limits.high_percent,
                limits.low_percent);
        }
        update_latch(total_latched,
            sum(used, false),
            dynamic_limit,
            limits.high_percent,
            limits.low_percent);
        for (std::size_t position = 0; position < used.size(); ++position) {
            peak[position].messages = std::max(peak[position].messages, used[position].messages);
            peak[position].bytes = std::max(peak[position].bytes, used[position].bytes);
        }
        const auto total = sum(used, true);
        peak_total.messages = std::max(peak_total.messages, total.messages);
        peak_total.bytes = std::max(peak_total.bytes, total.bytes);
    }

    void update_account(BudgetAccount& account) {
        for (std::size_t position = 1; position < account.used.size(); ++position) {
            update_latch(account.latched[position],
                account.used[position],
                local[position],
                limits.high_percent,
                limits.low_percent);
        }
    }

    void clear_wait(BudgetAccount& account) {
        std::erase(waiting, account.id);
        account.waiting = WaitKind::None;
    }

    void wait(BudgetAccount& account, WaitKind kind, std::size_t body, std::uint64_t epoch) {
        if (account.waiting != kind) {
            clear_wait(account);
            waiting.push_back(account.id);
        }
        account.waiting = kind;
        account.wanted_body = body;
        account.notified_epoch = epoch;
    }

    std::shared_ptr<BudgetAccount> candidate(WaitKind kind) const {
        for (const auto id : waiting) {
            const auto& account = accounts.at(id);
            const auto epoch = budget_epoch;
            if (account->waiting != kind || account->closed || !account->wakeup) {
                continue;
            }
            const auto relevant_epoch = kind == WaitKind::Queue ? queue_epoch : epoch;
            if (!account->scheduled && account->notified_epoch >= relevant_epoch) {
                continue;
            }
            if (kind == WaitKind::Queue || available(*account, account->wanted_body)) {
                return account;
            }
        }
        return nullptr;
    }

    std::optional<BudgetNotification> next_notification() {
        // 按登记顺序给可推进的等待者一轮机会；局部慢写不会挡住其他连接。
        auto account = candidate(WaitKind::Budget);
        if (!account) {
            account = candidate(WaitKind::Queue);
        }
        if (!account || account->scheduled) {
            return std::nullopt;
        }
        account->scheduled = true;
        account->notified_epoch = budget_epoch;
        if (account->waiting == WaitKind::Queue) {
            account->notified_epoch = queue_epoch;
        }
        ++pending_wakeups;
        BudgetNotification notification;
        notification.callback = account->wakeup;
        notification.ticket.state_ = shared_from_this();
        notification.ticket.account_ = account;
        return notification;
    }

    ResourceBudgetLimits limits;
    std::mutex mutex;
    std::array<ResourceUsage, 4> local{};
    std::array<ResourceUsage, 4> global{};
    ResourceUsage dynamic_limit;
    std::array<ResourceUsage, 4> used{};
    std::array<ResourceUsage, 4> peak{};
    ResourceUsage peak_total;
    std::array<bool, 4> latched{};
    bool total_latched = false;
    std::unordered_map<std::uint64_t, std::shared_ptr<BudgetAccount>> accounts;
    std::deque<std::uint64_t> waiting;
    std::uint64_t budget_epoch = 1;
    std::uint64_t queue_epoch = 1;
    std::size_t pending_wakeups = 0;
};

namespace {

void dispatch(std::optional<BudgetNotification> notification) noexcept {
    if (notification) {
        try {
            (*notification->callback)(std::move(notification->ticket));
        } catch (...) {
            // 若投递本身失败，凭证栈展开仍清除 pending；不能从资源析构抛异常。
        }
    }
}

} // namespace

ResourceLease::~ResourceLease() {
    release();
}

ResourceLease::ResourceLease(ResourceLease&& other) noexcept
    : state_(std::move(other.state_)),
      account_(std::move(other.account_)),
      resource_(other.resource_),
      amount_(std::exchange(other.amount_, {})) {
}

ResourceLease& ResourceLease::operator=(ResourceLease&& other) noexcept {
    if (this != &other) {
        release();
        state_ = std::move(other.state_);
        account_ = std::move(other.account_);
        resource_ = other.resource_;
        amount_ = std::exchange(other.amount_, {});
    }
    return *this;
}

ResourceLease::operator bool() const {
    return static_cast<bool>(state_);
}

std::size_t ResourceLease::bytes() const {
    return amount_.bytes;
}

void ResourceLease::shrink_bytes(std::size_t bytes) {
    if (!state_ || bytes > amount_.bytes) {
        throw std::logic_error("resource reservation cannot grow");
    }
    if (bytes == amount_.bytes) {
        return;
    }
    std::optional<BudgetNotification> notification;
    {
        std::lock_guard lock(state_->mutex);
        const ResourceUsage returned{0, amount_.bytes - bytes};
        subtract(state_->used[index(resource_)], returned);
        subtract(account_->used[index(resource_)], returned);
        amount_.bytes = bytes;
        state_->update_account(*account_);
        state_->update();
        ++state_->budget_epoch;
        notification = state_->next_notification();
    }
    dispatch(std::move(notification));
}

void ResourceLease::release() {
    auto state = std::move(state_);
    auto account = std::move(account_);
    if (!state) {
        return;
    }
    std::optional<BudgetNotification> notification;
    {
        std::lock_guard lock(state->mutex);
        subtract(state->used[index(resource_)], amount_);
        subtract(account->used[index(resource_)], amount_);
        amount_ = {};
        state->update_account(*account);
        state->update();
        ++state->budget_epoch;
        notification = state->next_notification();
    }
    dispatch(std::move(notification));
}

WakeupTicket::~WakeupTicket() {
    finish();
}

WakeupTicket::WakeupTicket(WakeupTicket&& other) noexcept
    : state_(std::move(other.state_)), account_(std::move(other.account_)) {
}

WakeupTicket& WakeupTicket::operator=(WakeupTicket&& other) noexcept {
    if (this != &other) {
        finish();
        state_ = std::move(other.state_);
        account_ = std::move(other.account_);
    }
    return *this;
}

void WakeupTicket::finish() {
    auto state = std::move(state_);
    auto account = std::move(account_);
    if (!state) {
        return;
    }
    std::optional<BudgetNotification> notification;
    {
        std::lock_guard lock(state->mutex);
        account->scheduled = false;
        --state->pending_wakeups;
        notification = state->next_notification();
    }
    dispatch(std::move(notification));
}

ResourceBudget::ResourceBudget(ResourceBudgetLimits limits) {
    const auto safe_max = std::numeric_limits<std::size_t>::max() / 100;
    const auto bounded = [safe_max](ResourceUsage capacity) {
        return capacity.messages > 0 && capacity.bytes > 0 && capacity.messages <= safe_max &&
               capacity.bytes <= safe_max;
    };
    if (limits.connections == 0 || limits.connections > safe_max / read_buffer_bytes ||
        limits.receive_body_bytes == 0 || limits.receive_body_bytes > safe_max ||
        !bounded(limits.receive) || !bounded(limits.business) || !bounded(limits.local_send) ||
        !bounded(limits.send) || !bounded(limits.total) || limits.total.messages < 3 ||
        limits.low_percent >= limits.high_percent || limits.high_percent > 99) {
        throw std::invalid_argument("invalid resource budgets or watermark percentages");
    }
    const auto static_bytes = limits.connections * read_buffer_bytes;
    const auto minimum_frame = 2 + request_reserve_padding + 2 * max_ack_frame_bytes;
    if (limits.total.bytes < static_bytes || limits.total.bytes - static_bytes < minimum_frame) {
        throw std::invalid_argument("global budget must reserve all read blocks and one frame");
    }
    state_ = std::make_shared<ResourceBudgetState>(limits);
}

std::optional<ConnectionBudget> ResourceBudget::open(std::uint64_t id) {
    std::lock_guard lock(state_->mutex);
    if (state_->accounts.size() >= state_->limits.connections || state_->accounts.contains(id) ||
        !fits(state_->used[0], {0, read_buffer_bytes}, state_->global[0])) {
        return std::nullopt;
    }
    auto account = std::make_shared<BudgetAccount>();
    account->id = id;
    state_->accounts.emplace(id, account);
    ConnectionBudget result;
    result.account = account;
    result.read.state_ = state_;
    result.read.account_ = account;
    result.read.resource_ = Resource::Read;
    result.read.amount_ = {0, read_buffer_bytes};
    add(account->used[0], result.read.amount_);
    add(state_->used[0], result.read.amount_);
    state_->update();
    return result;
}

void ResourceBudget::set_wakeup(const std::shared_ptr<BudgetAccount>& account, Wakeup wakeup) {
    std::lock_guard lock(state_->mutex);
    account->wakeup = std::make_shared<const Wakeup>(std::move(wakeup));
}

void ResourceBudget::close(const std::shared_ptr<BudgetAccount>& account) {
    std::optional<BudgetNotification> notification;
    {
        std::lock_guard lock(state_->mutex);
        if (account->closed) {
            return;
        }
        account->closed = true;
        state_->clear_wait(*account);
        state_->accounts.erase(account->id);
        notification = state_->next_notification();
    }
    dispatch(std::move(notification));
}

FrameAdmission ResourceBudget::try_frame(
    const std::shared_ptr<BudgetAccount>& account, std::size_t body_bytes) {
    FrameAdmission result;
    std::optional<BudgetNotification> notification;
    {
        std::lock_guard lock(state_->mutex);
        result.failure = state_->impossible(body_bytes);
        if (account->closed || result.failure != BudgetFailure::None) {
            result.status = BudgetStatus::Impossible;
            return result;
        }
        const auto first = state_->candidate(WaitKind::Budget);
        if (!state_->available(*account, body_bytes) || (first && first != account)) {
            // 登记与容量检查同锁；释放事件不可能丢在两者之间。
            state_->wait(*account, WaitKind::Budget, body_bytes, 0);
            notification = state_->next_notification();
        } else {
            state_->clear_wait(*account);
            auto attach = [&](ResourceLease& lease, Resource resource, ResourceUsage amount) {
                lease.state_ = state_;
                lease.account_ = account;
                lease.resource_ = resource;
                lease.amount_ = amount;
                add(state_->used[index(resource)], amount);
                add(account->used[index(resource)], amount);
            };
            attach(result.reservation.receive, Resource::Receive, {1, body_bytes});
            attach(result.reservation.task.business,
                Resource::Business,
                {1, body_bytes + request_reserve_padding + max_ack_frame_bytes});
            attach(result.reservation.task.reply, Resource::Send, {1, max_ack_frame_bytes});
            result.status = BudgetStatus::Acquired;
            state_->update_account(*account);
            state_->update();
            notification = state_->next_notification();
        }
    }
    dispatch(std::move(notification));
    return result;
}

bool ResourceBudget::allow_read(const std::shared_ptr<BudgetAccount>& account) {
    bool allowed = false;
    std::optional<BudgetNotification> notification;
    {
        std::lock_guard lock(state_->mutex);
        const auto first = state_->candidate(WaitKind::Budget);
        allowed = state_->available(*account, 0) && (!first || first == account);
        if (allowed) {
            state_->clear_wait(*account);
        } else if (!account->closed) {
            state_->wait(*account, WaitKind::Budget, 0, 0);
        }
        notification = state_->next_notification();
    }
    dispatch(std::move(notification));
    return allowed;
}

QueueTurn ResourceBudget::queue_turn(const std::shared_ptr<BudgetAccount>& account) {
    QueueTurn result{};
    std::optional<BudgetNotification> notification;
    {
        std::lock_guard lock(state_->mutex);
        result.epoch = state_->queue_epoch;
        const auto first = state_->candidate(WaitKind::Queue);
        result.allowed = !account->closed && (!first || first == account);
        if (!result.allowed && !account->closed) {
            state_->wait(*account, WaitKind::Queue, 0, state_->queue_epoch - 1);
            notification = state_->next_notification();
        }
    }
    dispatch(std::move(notification));
    return result;
}

void ResourceBudget::wait_queue(
    const std::shared_ptr<BudgetAccount>& account, std::uint64_t observed_epoch) {
    std::optional<BudgetNotification> notification;
    {
        std::lock_guard lock(state_->mutex);
        if (!account->closed) {
            state_->wait(*account, WaitKind::Queue, 0, observed_epoch);
            notification = state_->next_notification();
        }
    }
    dispatch(std::move(notification));
}

void ResourceBudget::queue_accepted(const std::shared_ptr<BudgetAccount>& account) {
    std::optional<BudgetNotification> notification;
    {
        std::lock_guard lock(state_->mutex);
        state_->clear_wait(*account);
        notification = state_->next_notification();
    }
    dispatch(std::move(notification));
}

void ResourceBudget::notify_task_capacity() const {
    std::optional<BudgetNotification> notification;
    {
        std::lock_guard lock(state_->mutex);
        ++state_->queue_epoch;
        notification = state_->next_notification();
    }
    dispatch(std::move(notification));
}

ResourceBudgetSnapshot ResourceBudget::snapshot() const {
    std::lock_guard lock(state_->mutex);
    ResourceBudgetSnapshot result;
    result.used = state_->used;
    result.peak = state_->peak;
    result.total = sum(state_->used, true);
    result.peak_total = state_->peak_total;
    result.pending_wakeups = state_->pending_wakeups;
    for (const auto& [id, account] : state_->accounts) {
        if (account->waiting == WaitKind::Budget) {
            ++result.budget_waiters;
        } else if (account->waiting == WaitKind::Queue) {
            ++result.queue_waiters;
        }
    }
    return result;
}

} // namespace gateway
