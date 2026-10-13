#include "server/server.h"
#include <charconv>
#include <fstream>
#include <iostream>
#include <limits>

namespace aegisdb::server {
namespace {
std::uint64_t number(std::string_view text) {
    std::uint64_t value{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    detail::require(parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size(),
                    Code::invalid_argument, "invalid option number");
    return value;
}
void apply(json::object object, ServerOptions &options) {
    auto integer = [&](const char *name, auto &field) {
        if (auto *v = object.if_contains(name)) {
            detail::require(v->is_uint64() || (v->is_int64() && v->as_int64() >= 0),
                            Code::invalid_argument, "invalid configuration number");
            const auto n =
                v->is_uint64() ? v->as_uint64() : static_cast<std::uint64_t>(v->as_int64());
            using T = std::remove_reference_t<decltype(field)>;
            detail::require(n <= static_cast<std::uint64_t>(std::numeric_limits<T>::max()),
                            Code::invalid_argument, "configuration number overflow");
            field = static_cast<T>(n);
        }
    };
    auto text = [&](const char *name, auto &field) {
        if (auto *v = object.if_contains(name)) {
            detail::require(v->is_string(), Code::invalid_argument, "invalid configuration string");
            field = std::string(v->as_string());
        }
    };
    text("host", options.host);
    text("database_directory", options.directory);
    integer("port", options.port);
    integer("io_threads", options.ioThreads);
    integer("workers", options.workers);
    integer("max_connections", options.maxConnections);
    integer("max_request_bytes", options.maxRequestBytes);
    integer("max_server_snapshots", options.maxServerSnapshots);
    integer("max_maintenance_operations", options.maxMaintenanceOperations);
    auto timeout = options.timeout.count(), ttl = options.snapshotTTL.count();
    integer("timeout_ms", timeout);
    integer("snapshot_ttl_seconds", ttl);
    options.timeout = std::chrono::milliseconds(timeout);
    options.snapshotTTL = std::chrono::seconds(ttl);
    if (auto *v = object.if_contains("storage")) {
        detail::require(v->is_object(), Code::invalid_argument,
                        "storage configuration must be object");
        object = v->as_object();
        auto &s = options.storage;
        integer("memtable_bytes", s.memtableBytes);
        integer("wal_segment_bytes", s.walSegmentBytes);
        integer("max_immutable_memtables", s.maxImmutableMemtables);
        integer("block_bytes", s.blockBytes);
        integer("table_bytes", s.tableBytes);
        integer("block_cache_bytes", s.blockCacheBytes);
        integer("max_key_bytes", s.maxKeyBytes);
        integer("max_value_bytes", s.maxValueBytes);
        integer("max_batch_bytes", s.maxBatchBytes);
        integer("max_batch_operations", s.maxBatchOperations);
        integer("max_scan_results", s.maxScanResults);
        integer("max_scan_bytes", s.maxScanBytes);
        integer("max_snapshots", s.maxSnapshots);
        integer("skip_list_height", s.skipListHeight);
        integer("level_count", s.levelCount);
        integer("level_one_bytes", s.levelOneBytes);
        integer("l0_compaction_files", s.l0CompactionFiles);
        integer("l0_stop_files", s.l0StopFiles);
        for (auto [name, field] : {std::pair{"level_ratio", &s.levelRatio},
                                   {"bloom_false_positive_rate", &s.bloomFalsePositiveRate},
                                   {"skip_list_promotion", &s.skipListPromotion}})
            if (auto *x = object.if_contains(name)) {
                detail::require(x->is_number(), Code::invalid_argument,
                                "invalid probability or ratio");
                *field = json::value_to<double>(*x);
            }
        for (auto [name, field] : {std::pair{"automatic_compaction", &s.automaticCompaction},
                                   {"verify_tables_on_open", &s.verifyTablesOnOpen}})
            if (auto *x = object.if_contains(name)) {
                detail::require(x->is_bool(), Code::invalid_argument,
                                "invalid configuration boolean");
                *field = x->as_bool();
            }
        auto writeTimeout = s.writeTimeout.count();
        integer("write_timeout_ms", writeTimeout);
        s.writeTimeout = std::chrono::milliseconds(writeTimeout);
    }
}
} // namespace
ServerOptions parseOptions(int argc, char **argv) {
    ServerOptions options;
    for (int i = 1; i < argc; ++i)
        if (std::string_view(argv[i]) == "--config") {
            detail::require(i + 1 < argc, Code::invalid_argument, "missing config path");
            std::ifstream file(argv[++i]);
            detail::require(bool(file), Code::invalid_argument, "cannot read config");
            detail::require(std::filesystem::file_size(argv[i]) <= 65536, Code::invalid_argument,
                            "config too large");
            std::string bytes((std::istreambuf_iterator<char>(file)), {});
            detail::require(bytes.size() <= 65536, Code::invalid_argument, "config too large");
            const auto value = json::parse(bytes);
            detail::require(value.is_object(), Code::invalid_argument, "invalid config object");
            apply(value.as_object(), options);
        }
    for (int i = 1; i < argc; ++i) {
        const std::string_view flag = argv[i];
        if (flag == "--config") {
            ++i;
            continue;
        }
        if (flag == "--help") {
            std::cout << "aegisdb-server [--config config/example.json] [--db PATH] [--host "
                         "ADDRESS] [--port N] [--workers N] [--io-threads N] [--max-connections N] "
                         "[--max-request-bytes N] [--timeout-ms N] [--memtable-bytes N]\n";
            std::exit(0);
        }
        detail::require(i + 1 < argc, Code::invalid_argument, "missing option value");
        const std::string_view value = argv[++i];
        json::object overrides;
        if (flag == "--host")
            options.host = value;
        else if (flag == "--db")
            options.directory = std::string(value);
        else if (flag == "--port")
            overrides["port"] = number(value);
        else if (flag == "--workers")
            overrides["workers"] = number(value);
        else if (flag == "--io-threads")
            overrides["io_threads"] = number(value);
        else if (flag == "--max-connections")
            overrides["max_connections"] = number(value);
        else if (flag == "--max-request-bytes")
            overrides["max_request_bytes"] = number(value);
        else if (flag == "--timeout-ms")
            overrides["timeout_ms"] = number(value);
        else if (flag == "--memtable-bytes")
            overrides["storage"] = json::object{{"memtable_bytes", number(value)}};
        else
            throw detail::Error(Code::invalid_argument, "unknown option");
        apply(std::move(overrides), options);
    }
    return validateServerOptions(std::move(options));
}
ServerOptions validateServerOptions(ServerOptions options) {
    detail::require(
        options.ioThreads > 0 && options.ioThreads <= 64 && options.workers > 0 &&
            options.workers <= 64 && options.maxConnections > 0 && options.maxConnections <= 4096 &&
            options.maxRequestBytes > 0 && options.maxRequestBytes <= 64 * 1024 * 1024 &&
            options.timeout.count() > 0 && options.timeout <= std::chrono::minutes(5) &&
            options.snapshotTTL.count() > 0 && options.snapshotTTL <= std::chrono::hours(24) &&
            options.maxServerSnapshots > 0 &&
            options.maxServerSnapshots <= options.storage.maxSnapshots &&
            options.maxMaintenanceOperations > 0 && options.maxMaintenanceOperations <= 1024,
        Code::invalid_argument, "invalid server resource limits");
    return options;
}
} // namespace aegisdb::server
