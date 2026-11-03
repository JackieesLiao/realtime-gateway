#include "gateway/server.hpp"

#include "gateway/buffered_input.hpp"
#include "gateway/log.hpp"
#include "gateway/send_queue.hpp"

#include <algorithm>
#include <csignal>
#include <functional>

namespace gateway {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using Error = boost::system::error_code;

namespace {

Dispatcher::Handler make_business_handler(Router& router, Dispatcher::Handler handler) {
    if (handler) {
        return handler;
    }

    return [&router](const realtime::Request& request) {
        return router.route(request);
    };
}

} // namespace

class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket,
        const Config& config,
        Dispatcher& dispatcher,
        ResourceBudget budgets,
        ConnectionBudget connection_budget,
        std::uint64_t connection_id,
        std::function<void()> on_close)
        : socket_(std::move(socket)),
          budgets_(std::move(budgets)),
          account_(std::move(connection_budget.account)),
          read_reservation_(std::move(connection_budget.read)),
          input_(config.max_body_bytes, config.max_receive_body_bytes),
          sends_(config.max_send_messages, config.max_send_bytes),
          dispatcher_(dispatcher),
          max_task_bytes_(std::min(
              config.business_queue.queued_bytes, config.business_queue.outstanding_bytes)),
          connection_id_(connection_id),
          on_close_(std::move(on_close)) {
    }

    void start() {
        const auto executor = socket_.get_executor();
        budgets_.set_wakeup(account_, [weak = weak_from_this(), executor](WakeupTicket ticket) {
            asio::post(executor, [weak, ticket = std::move(ticket)] {
                if (auto session = weak.lock()) {
                    session->resume_reading();
                }
                // ticket 析构清除 pending，推进下一位等待者；每连接至多一个唤醒。
            });
        });
        asio::dispatch(socket_.get_executor(), [self = shared_from_this()] {
            self->process_buffered_input();
        });
    }

    void stop() {
        asio::dispatch(socket_.get_executor(), [self = shared_from_this()] {
            self->close("server_stop");
        });
    }

    // M2.1：为后续异步业务提供控制入口，不等待业务线程，也不取消在途读。
    void pause_reading() {
        asio::dispatch(socket_.get_executor(), [self = shared_from_this()] {
            self->reading_paused_ = true;
        });
    }

    void resume_reading() {
        asio::dispatch(socket_.get_executor(), [self = shared_from_this()] {
            self->reading_paused_ = false;
            self->process_buffered_input();
        });
    }

private:
    void read() {
        if (closed_ || business_in_progress_ || reading_paused_ || read_in_progress_ ||
            !input_.can_read()) {
            return;
        }

        // 已准入的半帧必须可以收齐，不能被它自己占用的高水位卡住。
        if (!incoming_.receive && !budgets_.allow_read(account_)) {
            reading_paused_ = true;
            return;
        }

        // 整块缓冲已在接入时预留，覆盖在途 read 可能返回的全部数据。
        // 固定缓冲的所有权持续到读回调；旧输入未消费完时绝不覆盖它。
        const auto buffer = input_.writable_buffer();
        read_in_progress_ = true;
        socket_.async_read_some(asio::buffer(buffer.data(), buffer.size()),
            [self = shared_from_this()](Error error, std::size_t bytes_received) {
                self->handle_read(error, bytes_received);
            });
    }

    void handle_read(Error error, std::size_t bytes_received) {
        read_in_progress_ = false;
        if (closed_) {
            return;
        }

        if (error) {
            if (error == asio::error::eof) {
                close("peer_eof");
            } else {
                close("read_error");
            }
            return;
        }

        // 即使暂停已经设置，在途读返回的数据也先保存在固定缓冲中。
        input_.commit_read(bytes_received);
        process_buffered_input();
    }

    void process_buffered_input() {
        // 读操作持有可写缓冲时不能同时解析它；恢复请求等读回调继续推进。
        if (closed_ || read_in_progress_ || reading_paused_ || business_in_progress_) {
            return;
        }

        const auto status = input_.process(
            [this](const std::string& body) {
                if (reading_paused_ || business_in_progress_) {
                    return FrameDisposition::Paused;
                }
                return handle_body(body);
            },
            [this](std::size_t bytes) {
                return admit_frame(bytes);
            },
            [this] {
                // BufferedInput 已销毁消息体存储，现在才归还接收预算。
                incoming_.receive.release();
            });

        if (status == InputStatus::Paused) {
            reading_paused_ = true;
            return;
        }

        if (status == InputStatus::Error) {
            close(input_.error());
            return;
        }

        // 只有旧帧和原始尾部都已处理完，才允许启动下一条读操作。
        if (status == InputStatus::NeedRead) {
            read();
        }
    }

    bool admit_frame(std::size_t body_bytes) {
        auto admission = budgets_.try_frame(account_, body_bytes);
        if (admission.status == BudgetStatus::Acquired) {
            incoming_ = std::move(admission.reservation);
            return true;
        }
        if (admission.status == BudgetStatus::Impossible) {
            switch (admission.failure) {
            case BudgetFailure::Receive:
                close("receive_capacity");
                break;
            case BudgetFailure::Business:
                close("business_capacity");
                break;
            case BudgetFailure::Send:
                close("send_capacity");
                break;
            default:
                close("global_capacity");
                break;
            }
        }
        return false;
    }

    FrameDisposition handle_body(const std::string& body) {
        if (!pending_request_) {
            pending_request_ = std::make_unique<realtime::Request>();
            if (!pending_request_->ParseFromArray(body.data(), static_cast<int>(body.size()))) {
                close("invalid_protobuf");
                return FrameDisposition::Stopped;
            }
            const auto request_bytes = pending_request_->ByteSizeLong();
            if (request_bytes > max_task_bytes_ ||
                request_bytes + max_ack_frame_bytes > incoming_.task.business.bytes()) {
                close("business_capacity");
                return FrameDisposition::Stopped;
            }
            // 按包头保守预留，解析后只缩小；重复唤醒不再解析同一份 protobuf。
            incoming_.task.business.shrink_bytes(request_bytes + max_ack_frame_bytes);
        }

        const auto turn = budgets_.queue_turn(account_);
        if (!turn.allowed) {
            return FrameDisposition::Paused;
        }
        const auto status =
            dispatcher_.try_submit(pending_request_, make_completion(), &incoming_.task);
        if (status == SubmitStatus::Full) {
            // 资源和 Request 留在连接中；队列释放事件唤醒，无主动重试循环。
            budgets_.wait_queue(account_, turn.epoch);
            return FrameDisposition::Paused;
        }
        if (status == SubmitStatus::Stopping) {
            close("dispatcher_stopping");
            return FrameDisposition::Stopped;
        }

        budgets_.queue_accepted(account_);
        business_in_progress_ = true;
        pause_reading();
        return FrameDisposition::Accepted;
    }

    BusinessCompletion make_completion() {
        // 完成回调只弱引用连接；工作线程不访问任何 Session 成员。
        // guard 保证连接关闭后，io_context 仍能接收并丢弃迟到结果。
        const auto executor = socket_.get_executor();
        return [weak_session = weak_from_this(), executor, work = asio::make_work_guard(executor)](
                   BusinessResult result) mutable {
            asio::post(executor,
                [weak_session, work = std::move(work), result = std::move(result)]() mutable {
                    if (auto session = weak_session.lock()) {
                        session->handle_business_result(std::move(result));
                    }
                    // 弱引用失效时，result 析构同样归还总在途预算。
                });
        };
    }

    void handle_business_result(BusinessResult result) {
        // 这里只能运行在连接 strand。关闭后的结果直接丢弃，不再发送 ACK。
        if (closed_) {
            return;
        }

        business_in_progress_ = false;
        bool queued = false;
        {
            realtime::Ack ack;
            ack.Swap(&result.ack);
            log({{"event", "business_result"},
                {"connection_id", connection_id_},
                {"request_id", ack.request_id()},
                {"result", realtime::Result_Name(ack.result())}});
            queued = enqueue_ack(ack, std::move(result.reply_budget));
            // ACK 对象在此作用域结束时销毁，随后才能归还统一业务预算。
        }
        // 必须先归还旧请求凭证再恢复，否则容量为 1 时会拒绝自己的下一条请求。
        result.ticket.release();
        if (queued) {
            resume_reading();
        }
    }

    bool enqueue_ack(const realtime::Ack& ack, ResourceLease reply_budget) {
        const auto body_bytes = ack.ByteSizeLong();
        if (body_bytes == 0 || body_bytes > max_ack_frame_bytes - 4) {
            close("ack_too_large");
            return false;
        }
        // 直接写入已预留的线帧，避免 SerializeAsString 再复制一份消息体。
        std::string wire(body_bytes + 4, '\0');
        const auto length = static_cast<std::uint32_t>(body_bytes);
        for (unsigned byte = 0; byte < 4; ++byte) {
            wire[byte] = static_cast<char>((length >> (24 - 8 * byte)) & 0xff);
        }
        if (!ack.SerializeToArray(wire.data() + 4, static_cast<int>(body_bytes))) {
            close("ack_serialization_error");
            return false;
        }
        reply_budget.shrink_bytes(wire.size());
        auto frame =
            std::make_shared<const OutgoingFrame>(std::move(wire), std::move(reply_budget));
        const bool write_chain_idle = sends_.empty();
        if (!sends_.push(std::move(frame))) {
            close("send_capacity");
            return false;
        }

        if (write_chain_idle) {
            write();
        }
        return true;
    }

    void write() {
        // close() can clear the queue before cancellation completes. The callback must own
        // the active buffer separately, as well as keeping this Session alive.
        auto frame = sends_.front();
        asio::async_write(socket_,
            asio::buffer(frame->bytes),
            [self = shared_from_this(), frame](Error error, std::size_t) {
                self->handle_write(error);
            });
    }

    void handle_write(Error error) {
        if (closed_) {
            return;
        }

        if (error) {
            close("write_error");
            return;
        }

        sends_.pop();
        if (!sends_.empty()) {
            write();
        }
    }

    void close(const std::string& reason) {
        if (closed_) {
            return;
        }
        closed_ = true;
        input_.stop();
        budgets_.close(account_);

        Error ignored_error;
        socket_.cancel(ignored_error);
        socket_.shutdown(tcp::socket::shutdown_both, ignored_error);
        socket_.close(ignored_error);
        sends_.clear();

        log({{"event", "connection_closed"},
            {"connection_id", connection_id_},
            {"reason", reason}});
        on_close_();
    }

    // Every member below is accessed on the socket's dedicated connection strand.
    tcp::socket socket_;
    ResourceBudget budgets_;
    std::shared_ptr<BudgetAccount> account_;
    // 先声明凭证、后声明数据，保证逆序销毁时数据先释放。
    ResourceLease read_reservation_;
    FrameReservation incoming_;
    BufferedInput input_;
    std::unique_ptr<realtime::Request> pending_request_;
    SendQueue sends_;

    // main keeps Server (and its Dispatcher) alive until all I/O callbacks have drained.
    Dispatcher& dispatcher_;
    std::size_t max_task_bytes_;
    std::uint64_t connection_id_;
    std::function<void()> on_close_;
    bool business_in_progress_ = false;
    bool read_in_progress_ = false;
    bool reading_paused_ = false;
    bool closed_ = false;
};

Server::Server(asio::io_context& io, Config config, Dispatcher::Handler handler)
    : io_(io),
      strand_(asio::make_strand(io)),
      acceptor_(strand_),
      signals_(strand_, SIGINT, SIGTERM),
      config_(std::move(config)),
      router_(stats_),
      budgets_(config_.budget_limits()),
      dispatcher_(config_.business_threads,
          config_.task_queue_limits(),
          make_business_handler(router_, std::move(handler))) {
    const tcp::endpoint endpoint(asio::ip::make_address(config_.listen_address), config_.port);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
    port_ = acceptor_.local_endpoint().port();
    dispatcher_.set_capacity_notifier([budgets = budgets_] {
        budgets.notify_task_capacity();
    });
}

void Server::start() {
    asio::dispatch(strand_, [self = shared_from_this()] {
        self->signals_.async_wait([self](Error error, int) {
            if (!error) {
                self->stop();
            }
        });

        log({{"event", "listening"},
            {"address", self->config_.listen_address},
            {"port", self->port_},
            {"io_threads", self->config_.io_threads}});
        self->accept();
    });
}

void Server::accept() {
    auto socket = std::make_shared<tcp::socket>(asio::make_strand(io_));
    acceptor_.async_accept(*socket, [self = shared_from_this(), socket](Error error) {
        self->handle_accept(error, socket);
    });
}

void Server::handle_accept(Error error, const std::shared_ptr<tcp::socket>& socket) {
    if (stopped_) {
        return;
    }

    if (error) {
        log({{"event", "accept_error"}, {"code", error.value()}});
        // Avoid a busy retry loop on persistent resource errors.
        stop();
        return;
    }

    if (sessions_.size() >= config_.max_connections) {
        Error ignored_error;
        socket->close(ignored_error);
        log({{"event", "connection_rejected"}, {"reason", "connection_capacity"}});
        accept();
        return;
    }

    const auto connection_id = next_id_++;
    const std::weak_ptr<Server> weak_server = shared_from_this();

    // Avoid a Server -> Session -> Server ownership cycle. Registry changes return to
    // the management strand, rather than running on the connection strand.
    auto on_close = [weak_server, connection_id] {
        if (auto server = weak_server.lock()) {
            asio::post(server->strand_, [server, connection_id] {
                server->sessions_.erase(connection_id);
            });
        }
    };

    auto connection_budget = budgets_.open(connection_id);
    if (!connection_budget) {
        Error ignored_error;
        socket->close(ignored_error);
        log({{"event", "connection_rejected"}, {"reason", "global_read_capacity"}});
        accept();
        return;
    }
    auto session = std::make_shared<Session>(std::move(*socket),
        config_,
        dispatcher_,
        budgets_,
        std::move(*connection_budget),
        connection_id,
        std::move(on_close));
    sessions_.emplace(connection_id, session);
    session->start();
    accept();
}

void Server::report_statistics() const {
    const auto business = dispatcher_.snapshot();
    const auto budget = budgets_.snapshot();
    log({{"event", "statistics"},
        {"processed", stats_.processed.load()},
        {"rejected", stats_.rejected.load()},
        {"payload_bytes", stats_.payload_bytes.load()},
        {"business_queued", business.queued_messages},
        {"business_outstanding", business.outstanding_messages},
        {"business_outstanding_bytes", business.outstanding_bytes},
        {"business_failed", dispatcher_.failed_tasks()},
        {"completion_failed", dispatcher_.failed_completions()},
        {"budget_messages", budget.total.messages},
        {"budget_bytes", budget.total.bytes},
        {"budget_waiters", budget.budget_waiters + budget.queue_waiters},
        {"budget_pending_wakeups", budget.pending_wakeups}});
}

void Server::join_workers() {
    dispatcher_.stop();
    dispatcher_.join();
}

std::uint16_t Server::port() const {
    return port_;
}

TaskQueueSnapshot Server::business_snapshot() const {
    return dispatcher_.snapshot();
}

ResourceBudgetSnapshot Server::budget_snapshot() const {
    return budgets_.snapshot();
}

void Server::stop() {
    asio::dispatch(strand_, [self = shared_from_this()] {
        if (self->stopped_) {
            return;
        }
        self->stopped_ = true;

        Error ignored_error;
        self->acceptor_.close(ignored_error);
        self->signals_.cancel(ignored_error);

        for (const auto& [connection_id, session] : self->sessions_) {
            session->stop();
        }

        // M2.2 仍立即取消连接和未执行任务；不在 I/O 回调内 join。
        self->dispatcher_.stop();
        log({{"event", "stop_requested"}});
    });
}

} // namespace gateway
