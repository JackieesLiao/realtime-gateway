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
        if (closed_ || reading_paused_ || read_in_progress_ || !input_.can_read()) {
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
        if (closed_ || read_in_progress_ || reading_paused_) {
            return;
        }

        const auto status = input_.process([this](const std::string& body) {
            if (reading_paused_) {
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
        realtime::Request request;
        if (!request.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
            close("invalid_protobuf");
            return false;
        }

        // Create the ACK only after business validation and counter updates complete.
        const auto ack = dispatcher_.dispatch(request);
        log({{"event", "business_result"},
            {"connection_id", connection_id_},
            {"request_id", request.request_id()},
            {"result", realtime::Result_Name(ack.result())}});

        const auto ack_body = ack.SerializeAsString();
        auto frame = std::make_shared<const std::string>(encode_frame(ack_body));
        const bool write_chain_idle = sends_.empty();

        if (!sends_.push(std::move(frame))) {
            close("send_capacity");
            return false;
        }

        // Otherwise the active write's completion callback will send this frame.
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
    bool read_in_progress_ = false;
    bool reading_paused_ = false;
    bool closed_ = false;
};

Server::Server(asio::io_context& io, Config config)
    : io_(io),
      strand_(asio::make_strand(io)),
      acceptor_(strand_),
      signals_(strand_, SIGINT, SIGTERM),
      config_(std::move(config)),
      dispatcher_(stats_) {
    const tcp::endpoint endpoint(asio::ip::make_address(config_.listen_address), config_.port);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
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
            {"port", self->acceptor_.local_endpoint().port()},
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
    log({{"event", "statistics"},
        {"processed", stats_.processed.load()},
        {"rejected", stats_.rejected.load()},
        {"payload_bytes", stats_.payload_bytes.load()}});
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

        log({{"event", "stop_requested"}});
    });
}

} // namespace gateway
