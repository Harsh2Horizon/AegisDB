#pragma once
#include <aegisdb/db.h>
#include <boost/asio.hpp>
#include <boost/beast.hpp>
#include <boost/json.hpp>
#include <condition_variable>
#include <map>

namespace aegisdb::server {
namespace net = boost::asio;
namespace beast = boost::beast;
namespace http = beast::http;
namespace json = boost::json;
using tcp = net::ip::tcp;

struct ServerOptions {
    std::string host{"127.0.0.1"};
    std::uint16_t port{8080};
    std::filesystem::path directory{"aegis-data"};
    std::size_t ioThreads{2}, workers{4}, maxConnections{64}, maxRequestBytes{32 * 1024 * 1024},
        maxServerSnapshots{128}, maxMaintenanceOperations{64};
    std::chrono::milliseconds timeout{10000};
    std::chrono::seconds snapshotTTL{300};
    Options storage;
};

using Request = http::request<http::string_body>;
using Response = http::response<http::string_body>;

class ApiRouter {
  public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;
    ApiRouter(Database &, ServerOptions, Clock = [] { return std::chrono::steady_clock::now(); });
    ~ApiRouter();
    Response route(const Request &);
    void expireSnapshots();
    void stopAdmission();
    void shutdown();

  private:
    struct SnapshotEntry {
        Snapshot handle;
        std::chrono::steady_clock::time_point expires;
    };
    struct Operation {
        std::string state{"queued"};
        Code result{Code::ok};
    };
    Snapshot lookupSnapshot(const json::object &);
    Response maintenance(bool compact);
    Database &db_;
    ServerOptions options_;
    Clock clock_;
    net::thread_pool maintenance_{1};
    std::mutex admissionMutex_, registryMutex_;
    bool stopping_{};
    std::map<std::string, SnapshotEntry> snapshots_;
    std::map<std::string, Operation> operations_;
    std::string sessionPrefix_;
    std::uint64_t nextSnapshot_{1}, nextOperation_{1};
};

class DatabaseServer {
  public:
    DatabaseServer(Database &, ServerOptions);
    void run();
    std::uint16_t port() const { return acceptor_.local_endpoint().port(); }

  private:
    class Session;
    void accept();
    void expire();
    void stop();
    bool submit(std::function<void()>);
    ServerOptions options_;
    ApiRouter router_;
    // Sessions can be destroyed by io_context teardown, so their counters outlive io_.
    std::atomic<std::size_t> connections_{};
    std::atomic<bool> stopping_{};
    net::io_context io_;
    net::strand<net::io_context::executor_type> strand_;
    tcp::acceptor acceptor_;
    net::signal_set signals_;
    net::steady_timer expiration_;
    net::thread_pool workers_;
    std::mutex admissionMutex_, stopMutex_;
    std::condition_variable stoppedCV_;
    bool stopped_{};
    std::vector<std::weak_ptr<Session>> sessions_;
};

ServerOptions parseOptions(int argc, char **argv);
ServerOptions validateServerOptions(ServerOptions);
Response errorResponse(Code code, unsigned version = 11);

} // namespace aegisdb::server
