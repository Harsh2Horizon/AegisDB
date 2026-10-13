#include "server/server.h"
#include <csignal>
#include <iostream>

namespace aegisdb::server {
class DatabaseServer::Session : public std::enable_shared_from_this<Session> {
  public:
    Session(tcp::socket socket, DatabaseServer &server)
        : stream_(std::move(socket)), server_(server) {
        ++server_.connections_;
        parser_.body_limit(server_.options_.maxRequestBytes);
        parser_.header_limit(16 * 1024);
    }
    ~Session() { --server_.connections_; }
    void start() {
        stream_.expires_after(server_.options_.timeout);
        http::async_read(stream_, buffer_, parser_,
                         beast::bind_front_handler(&Session::read, shared_from_this()));
    }
    void cancel() {
        net::dispatch(stream_.get_executor(), [self = shared_from_this()] {
            beast::error_code ec;
            self->stream_.socket().cancel(ec);
            self->stream_.socket().close(ec);
        });
    }

  private:
    void read(beast::error_code ec, std::size_t) {
        if (ec) {
            if (ec == http::error::body_limit) {
                auto r = errorResponse(Code::resource_exhausted);
                r.result(http::status::payload_too_large);
                send(std::move(r));
            } else if (ec != beast::error::timeout && ec != net::error::operation_aborted &&
                       ec != http::error::end_of_stream)
                send(errorResponse(Code::invalid_argument));
            return;
        }
        auto request = parser_.release();
        const auto self = shared_from_this();
        if (!server_.submit([self, request = std::move(request)] {
                auto result = self->server_.router_.route(request);
                net::post(self->stream_.get_executor(),
                          [self, result = std::move(result)]() mutable {
                              self->send(std::move(result));
                          });
            }))
            send(errorResponse(Code::shutting_down));
    }
    void send(Response result) {
        response_ = std::move(result);
        stream_.expires_after(server_.options_.timeout);
        http::async_write(stream_, *response_,
                          [self = shared_from_this()](beast::error_code, std::size_t) {
                              beast::error_code ignored;
                              self->stream_.socket().shutdown(tcp::socket::shutdown_both, ignored);
                              self->stream_.socket().close(ignored);
                          });
    }
    beast::tcp_stream stream_;
    DatabaseServer &server_;
    beast::flat_buffer buffer_;
    http::request_parser<http::string_body> parser_;
    std::optional<Response> response_;
};
DatabaseServer::DatabaseServer(Database &db, ServerOptions options)
    : options_(validateServerOptions(std::move(options))), router_(db, options_),
      strand_(net::make_strand(io_)), acceptor_(strand_), signals_(strand_, SIGINT, SIGTERM),
      expiration_(strand_), workers_(options_.workers) {
    const auto address = net::ip::make_address(options_.host);
    tcp::endpoint endpoint{address, options_.port};
    acceptor_.open(endpoint.protocol());
    acceptor_.set_option(net::socket_base::reuse_address(true));
    acceptor_.bind(endpoint);
    acceptor_.listen(net::socket_base::max_listen_connections);
}
bool DatabaseServer::submit(std::function<void()> task) {
    std::lock_guard lock(admissionMutex_);
    if (stopping_)
        return false;
    net::post(workers_, std::move(task));
    return true;
}
void DatabaseServer::accept() {
    acceptor_.async_accept(
        net::make_strand(io_),
        net::bind_executor(strand_, [this](beast::error_code ec, tcp::socket socket) {
            if (!ec && !stopping_) {
                std::erase_if(sessions_, [](const auto &weak) { return weak.expired(); });
                if (connections_ < options_.maxConnections) {
                    auto session = std::make_shared<Session>(std::move(socket), *this);
                    sessions_.push_back(session);
                    session->start();
                } else {
                    beast::error_code ignored;
                    socket.close(ignored);
                }
            }
            if (!stopping_ && acceptor_.is_open())
                accept();
        }));
}
void DatabaseServer::expire() {
    expiration_.expires_after(std::chrono::seconds(1));
    expiration_.async_wait([this](beast::error_code ec) {
        if (ec || stopping_)
            return;
        router_.expireSnapshots();
        expire();
    });
}
void DatabaseServer::stop() {
    {
        std::lock_guard lock(admissionMutex_);
        stopping_ = true;
    }
    router_.stopAdmission();
    beast::error_code ec;
    acceptor_.cancel(ec);
    acceptor_.close(ec);
    signals_.cancel(ec);
    expiration_.cancel();
    for (const auto &weak : sessions_)
        if (auto s = weak.lock())
            s->cancel();
    {
        std::lock_guard lock(stopMutex_);
        stopped_ = true;
    }
    stoppedCV_.notify_all();
}
void DatabaseServer::run() {
    auto guard = net::make_work_guard(io_);
    accept();
    expire();
    signals_.async_wait([this](beast::error_code ec, int) {
        if (!ec)
            stop();
    });
    std::cout << "AEGISDB_READY " << port() << "\n" << std::flush;
    std::vector<std::jthread> threads;
    threads.reserve(options_.ioThreads);
    try {
        for (std::size_t i = 0; i < options_.ioThreads; ++i)
            threads.emplace_back([this] {
                for (;;) {
                    try {
                        io_.run();
                        break;
                    } catch (...) {
                        std::cerr << "AegisDB: network worker failed\n";
                        net::post(strand_, [this] { stop(); });
                        // Re-enter run so a one-thread configuration can execute its stop handler.
                    }
                }
            });
    } catch (...) {
        // A partially constructed thread vector must not join while its work guard remains active.
        guard.reset();
        io_.stop();
        for (auto &thread : threads)
            thread.join();
        throw;
    }
    {
        std::unique_lock lock(stopMutex_);
        stoppedCV_.wait(lock, [this] { return stopped_; });
    }
    workers_.join();
    router_.shutdown();
    guard.reset();
    // Drain cancellation and response completions before destroying the engine facade.
    for (auto &t : threads)
        t.join();
}
} // namespace aegisdb::server
