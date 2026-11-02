#include "gateway/server.hpp"
#include "gateway/frame.hpp"
#include "gateway/send_queue.hpp"
#include "gateway/log.hpp"
#include <array>
#include <csignal>
#include <functional>

namespace gateway {
namespace asio = boost::asio;
using tcp = asio::ip::tcp;
using Error = boost::system::error_code;
class Session : public std::enable_shared_from_this<Session> {
public:
    Session(tcp::socket socket, const Config& config, Dispatcher& dispatcher,
            std::uint64_t id, std::function<void()> on_close)
        : socket_(std::move(socket)), decoder_(config.max_body_bytes, config.max_receive_body_bytes),
          sends_(config.max_send_messages, config.max_send_bytes), dispatcher_(dispatcher),
          id_(id), on_close_(std::move(on_close)) {}
    void start() {
        asio::dispatch(socket_.get_executor(), [self = shared_from_this()] { self->read(); });
    }
    void stop() {
        asio::dispatch(socket_.get_executor(), [self = shared_from_this()] { self->close("server_stop"); });
    }
private:
    void read() {
        if (closed_) return;
        socket_.async_read_some(asio::buffer(read_buffer_),
            [self = shared_from_this()](Error ec, std::size_t n) {
                if (self->closed_) return;
                if (ec) { self->close(ec == asio::error::eof ? "peer_eof" : "read_error"); return; }
                const bool ok = self->decoder_.feed({self->read_buffer_.data(), n},
                    [self](const std::string& body) { return self->handle_body(body); });
                if (!ok) {
                    if (!self->closed_) self->close(self->decoder_.error());
                    return;
                }
                self->read();
            });
    }
    bool handle_body(const std::string& body) {
        realtime::Request request;
        if (!request.ParseFromArray(body.data(), static_cast<int>(body.size()))) {
            close("invalid_protobuf");
            return false;
        }
        // ACK exists only after all business validation and counter updates complete.
        const auto ack = dispatcher_.dispatch(request);
        log({{"event", "business_result"}, {"connection_id", id_},
             {"request_id", request.request_id()}, {"result", realtime::Result_Name(ack.result())}});
        auto frame = std::make_shared<const std::string>(encode_frame(ack.SerializeAsString()));
        const bool idle = sends_.empty();
        if (!sends_.push(std::move(frame))) { close("send_capacity"); return false; }
        if (idle) write();
        return true;
    }
    void write() {
        // Capture the buffer separately: close() may clear the queue before cancellation completes.
        auto frame = sends_.front();
        asio::async_write(socket_, asio::buffer(*frame),
            [self = shared_from_this(), frame](Error ec, std::size_t) {
                if (self->closed_) return;
                if (ec) { self->close("write_error"); return; }
                self->sends_.pop();
                if (!self->sends_.empty()) self->write();
            });
    }
    void close(const std::string& reason) {
        if (closed_) return;
        closed_ = true;
        Error ignored;
        socket_.cancel(ignored);
        socket_.shutdown(tcp::socket::shutdown_both, ignored);
        socket_.close(ignored);
        sends_.clear();
        log({{"event", "connection_closed"}, {"connection_id", id_}, {"reason", reason}});
        on_close_();
    }
    tcp::socket socket_; // executor is a dedicated connection strand
    std::array<char, 4096> read_buffer_{};
    FrameDecoder decoder_;
    SendQueue sends_;
    Dispatcher& dispatcher_;
    std::uint64_t id_;
    std::function<void()> on_close_;
    bool closed_ = false;
};
Server::Server(asio::io_context& io, Config config)
    : io_(io), strand_(asio::make_strand(io)), acceptor_(strand_),
      signals_(strand_, SIGINT, SIGTERM), config_(std::move(config)), dispatcher_(stats_) {
    const tcp::endpoint endpoint(asio::ip::make_address(config_.listen_address), config_.port);
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(tcp::acceptor::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen();
}
void Server::start() {
    asio::dispatch(strand_, [self = shared_from_this()] {
        self->signals_.async_wait([self](Error ec, int) { if (!ec) self->stop(); });
        log({{"event", "listening"}, {"address", self->config_.listen_address},
             {"port", self->acceptor_.local_endpoint().port()}, {"io_threads", self->config_.io_threads}});
        self->accept();
    });
}
void Server::accept() {
    auto socket = std::make_shared<tcp::socket>(asio::make_strand(io_));
    acceptor_.async_accept(*socket, [self = shared_from_this(), socket](Error ec) {
        if (self->stopped_) return;
        if (ec) {
            log({{"event", "accept_error"}, {"code", ec.value()}});
            self->stop(); // avoid a busy retry loop on persistent resource errors
            return;
        }
        if (self->sessions_.size() >= self->config_.max_connections) {
            Error ignored; socket->close(ignored);
            log({{"event", "connection_rejected"}, {"reason", "connection_capacity"}});
        } else {
            const auto id = self->next_id_++;
            const std::weak_ptr<Server> weak = self;
            auto session = std::make_shared<Session>(std::move(*socket), self->config_, self->dispatcher_, id, [weak, id] {
                if (auto server = weak.lock()) {
                    asio::post(server->strand_, [server, id] { server->sessions_.erase(id); });
                }
            });
            self->sessions_.emplace(id, session);
            session->start();
        }
        self->accept();
    });
}
void Server::report_statistics() const {
    log({{"event", "statistics"}, {"processed", stats_.processed.load()},
         {"rejected", stats_.rejected.load()}, {"payload_bytes", stats_.payload_bytes.load()}});
}
void Server::stop() {
    asio::dispatch(strand_, [self = shared_from_this()] {
        if (self->stopped_) return;
        self->stopped_ = true;
        Error ignored;
        self->acceptor_.close(ignored);
        self->signals_.cancel(ignored);
        for (const auto& [id, session] : self->sessions_) { (void)id; session->stop(); }
        log({{"event", "stop_requested"}});
    });
}
}
