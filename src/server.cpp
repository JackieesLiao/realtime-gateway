#include "gateway/server.hpp"

#include "gateway/buffered_input.hpp"
#include "gateway/log.hpp"
#include "gateway/send_queue.hpp"

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
        std::uint64_t connection_id,
        std::function<void()> on_close)
        : socket_(std::move(socket)),
          input_(config.max_body_bytes, config.max_receive_body_bytes),
          sends_(config.max_send_messages, config.max_send_bytes),
          dispatcher_(dispatcher),
          connection_id_(connection_id),
          on_close_(std::move(on_close)) {
    }

    void start() {
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

        const auto status = input_.process([this](const std::string& body) {
            if (reading_paused_ || business_in_progress_) {
                return FrameDisposition::Paused;
            }
            if (!handle_body(body)) {
                return FrameDisposition::Stopped;
            }

            return FrameDisposition::Accepted;
        });

        if (status == InputStatus::Error) {
            close(input_.error());
            return;
        }

        // 只有旧帧和原始尾部都已处理完，才允许启动下一条读操作。
        if (status == InputStatus::NeedRead) {
            read();
        }
    }

    bool handle_body(const std::string& body) {
        auto request = std::make_unique<realtime::Request>();
        if (!request->ParseFromArray(body.data(), static_cast<int>(body.size()))) {
            close("invalid_protobuf");
            return false;
        }

        const auto status = dispatcher_.try_submit(request, make_completion());
        if (status == SubmitStatus::Full) {
            close("business_capacity");
            return false;
        }
        if (status == SubmitStatus::Stopping) {
            close("dispatcher_stopping");
            return false;
        }

        // 同连接只有一个待处理业务；后续完整帧/尾部由 M2.1 输入状态保留。
        business_in_progress_ = true;
        pause_reading();
        return true;
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
        log({{"event", "business_result"},
            {"connection_id", connection_id_},
            {"request_id", result.ack.request_id()},
            {"result", realtime::Result_Name(result.ack.result())}});

        const bool queued = enqueue_ack(result.ack);
        // 必须先归还旧请求凭证再恢复，否则容量为 1 时会拒绝自己的下一条请求。
        result.ticket.release();
        if (queued) {
            resume_reading();
        }
    }

    bool enqueue_ack(const realtime::Ack& ack) {
        auto frame = std::make_shared<const std::string>(encode_frame(ack.SerializeAsString()));
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
            asio::buffer(*frame),
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
    BufferedInput input_;
    SendQueue sends_;

    // main keeps Server (and its Dispatcher) alive until all I/O callbacks have drained.
    Dispatcher& dispatcher_;
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
      dispatcher_(config_.business_threads,
          config_.business_queue,
          make_business_handler(router_, std::move(handler))) {
    const tcp::endpoint endpoint(asio::ip::make_address(config_.listen_address), config_.port);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
    port_ = acceptor_.local_endpoint().port();
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

    auto session = std::make_shared<Session>(
        std::move(*socket), config_, dispatcher_, connection_id, std::move(on_close));
    sessions_.emplace(connection_id, session);
    session->start();
    accept();
}

void Server::report_statistics() const {
    const auto business = dispatcher_.snapshot();
    log({{"event", "statistics"},
        {"processed", stats_.processed.load()},
        {"rejected", stats_.rejected.load()},
        {"payload_bytes", stats_.payload_bytes.load()},
        {"business_queued", business.queued_messages},
        {"business_outstanding", business.outstanding_messages},
        {"business_outstanding_bytes", business.outstanding_bytes},
        {"business_failed", dispatcher_.failed_tasks()},
        {"completion_failed", dispatcher_.failed_completions()}});
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
