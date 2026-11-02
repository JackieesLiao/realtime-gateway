#include "gateway/server.hpp"

#include "gateway/frame.hpp"
#include "gateway/log.hpp"
#include "gateway/send_queue.hpp"

#include <array>
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
          decoder_(config.max_body_bytes, config.max_receive_body_bytes),
          sends_(config.max_send_messages, config.max_send_bytes),
          dispatcher_(dispatcher),
          connection_id_(connection_id),
          on_close_(std::move(on_close)) {
    }

    void start() {
        asio::dispatch(socket_.get_executor(), [self = shared_from_this()] {
            self->read();
        });
    }

    void stop() {
        asio::dispatch(socket_.get_executor(), [self = shared_from_this()] {
            self->close("server_stop");
        });
    }

private:
    void read() {
        if (closed_) {
            return;
        }

        socket_.async_read_some(asio::buffer(read_buffer_),
            [self = shared_from_this()](Error error, std::size_t bytes_received) {
                self->handle_read(error, bytes_received);
            });
    }

    void handle_read(Error error, std::size_t bytes_received) {
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

        const std::span<const char> received_bytes(read_buffer_.data(), bytes_received);
        const bool decoding_succeeded =
            decoder_.feed(received_bytes, [this](const std::string& body) {
                return handle_body(body);
            });

        if (!decoding_succeeded) {
            // A consumer rejection may already have closed this connection.
            if (!closed_) {
                close(decoder_.error());
            }
            return;
        }

        // Start another read only after every complete buffered frame has been consumed.
        read();
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
    std::array<char, 4096> read_buffer_{};
    FrameDecoder decoder_;
    SendQueue sends_;

    // main keeps Server (and its Dispatcher) alive until all I/O callbacks have drained.
    Dispatcher& dispatcher_;
    std::uint64_t connection_id_;
    std::function<void()> on_close_;
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
