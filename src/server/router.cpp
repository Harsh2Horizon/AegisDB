#include "server/base64.h"
#include "server/server.h"
#include <algorithm>
#include <csignal>
#include <iostream>
#include <random>

namespace aegisdb::server {
namespace {
Response response(http::status status, const json::value &body, unsigned version = 11) {
    Response r{status, version};
    r.set(http::field::content_type, "application/json");
    r.set(http::field::server, "AegisDB/0.1");
    r.keep_alive(false);
    r.body() = json::serialize(body);
    r.prepare_payload();
    return r;
}
const char *codeName(Code code) {
    switch (code) {
    case Code::ok:
        return "OK";
    case Code::not_found:
        return "NOT_FOUND";
    case Code::invalid_argument:
        return "INVALID_ARGUMENT";
    case Code::io_error:
        return "IO_ERROR";
    case Code::corruption:
        return "CORRUPTION";
    case Code::resource_exhausted:
        return "RESOURCE_EXHAUSTED";
    case Code::timeout:
        return "TIMEOUT";
    case Code::busy:
        return "BUSY";
    case Code::shutting_down:
        return "SHUTTING_DOWN";
    case Code::internal_error:
        return "INTERNAL_ERROR";
    }
    return "INTERNAL_ERROR";
}
std::string_view stringField(const json::object &o, std::string_view name) {
    const auto *v = o.if_contains(name);
    detail::require(v && v->is_string(), Code::invalid_argument, "missing string field");
    const auto &s = v->as_string();
    return {s.data(), s.size()};
}
bool booleanField(const json::object &o, std::string_view name, bool fallback) {
    const auto *v = o.if_contains(name);
    if (!v)
        return fallback;
    detail::require(v->is_bool(), Code::invalid_argument, "invalid boolean field");
    return v->as_bool();
}
std::uint64_t numberField(const json::object &o, std::string_view name, std::uint64_t fallback) {
    const auto *v = o.if_contains(name);
    if (!v)
        return fallback;
    detail::require(v->is_uint64() || (v->is_int64() && v->as_int64() >= 0), Code::invalid_argument,
                    "invalid numeric field");
    return v->is_uint64() ? v->as_uint64() : static_cast<std::uint64_t>(v->as_int64());
}
json::object statsJSON(const Statistics &s) {
    json::object o;
    o["puts"] = s.puts;
    o["gets"] = s.gets;
    o["deletes"] = s.deletes;
    o["scans"] = s.scans;
    o["read_hits"] = s.readHits;
    o["read_misses"] = s.readMisses;
    o["wal_bytes"] = s.walBytes;
    o["wal_syncs"] = s.walSyncs;
    o["memtable_bytes"] = s.memtableBytes;
    o["immutable_memtables"] = s.immutableMemtables;
    o["bloom_checks"] = s.bloomChecks;
    o["bloom_negatives"] = s.bloomNegatives;
    o["cache_hits"] = s.cacheHits;
    o["cache_misses"] = s.cacheMisses;
    o["cache_bytes"] = s.cacheBytes;
    o["flushes"] = s.flushes;
    o["flush_bytes_written"] = s.flushBytesWritten;
    o["compactions"] = s.compactions;
    o["compaction_bytes_read"] = s.compactionBytesRead;
    o["compaction_bytes_written"] = s.compactionBytesWritten;
    o["background_errors"] = s.backgroundErrors;
    o["last_sequence"] = s.lastSequence;
    json::array levels;
    for (const auto &l : s.levels)
        levels.emplace_back(json::object{{"tables", l.tables}, {"bytes", l.bytes}});
    o["levels"] = std::move(levels);
    return o;
}
} // namespace
Response errorResponse(Code code, unsigned version) {
    http::status status = http::status::internal_server_error;
    switch (code) {
    case Code::not_found:
        status = http::status::not_found;
        break;
    case Code::invalid_argument:
        status = http::status::bad_request;
        break;
    case Code::resource_exhausted:
        status = http::status::too_many_requests;
        break;
    case Code::timeout:
        status = http::status::request_timeout;
        break;
    case Code::busy:
    case Code::shutting_down:
        status = http::status::service_unavailable;
        break;
    default:
        break;
    }
    return response(status,
                    json::object{{"success", false},
                                 {"error", json::object{{"code", codeName(code)},
                                                        {"message", codeName(code)}}}},
                    version);
}
ApiRouter::ApiRouter(Database &db, ServerOptions options, Clock clock)
    : db_(db), options_(validateServerOptions(std::move(options))), clock_(std::move(clock)) {
    // Qualify handles by a fresh 128-bit generation to prevent reuse across server restarts.
    std::random_device random;
    constexpr char hex[] = "0123456789abcdef";
    for (int word = 0; word < 4; ++word) {
        const auto value = static_cast<std::uint32_t>(random());
        for (unsigned nibble = 0; nibble < 8; ++nibble)
            sessionPrefix_.push_back(hex[(value >> (nibble * 4)) & 15]);
    }
    sessionPrefix_ += '-';
}
ApiRouter::~ApiRouter() { shutdown(); }
void ApiRouter::stopAdmission() {
    std::lock_guard lock(admissionMutex_);
    stopping_ = true;
}
void ApiRouter::shutdown() {
    stopAdmission();
    maintenance_.join();
}
void ApiRouter::expireSnapshots() {
    std::lock_guard lock(registryMutex_);
    const auto now = clock_();
    std::erase_if(snapshots_, [now](const auto &entry) { return entry.second.expires <= now; });
}
Snapshot ApiRouter::lookupSnapshot(const json::object &object) {
    if (!object.if_contains("snapshot_id"))
        return {};
    const auto id = std::string(stringField(object, "snapshot_id"));
    std::lock_guard lock(registryMutex_);
    auto it = snapshots_.find(id);
    detail::require(it != snapshots_.end() && it->second.expires > clock_(), Code::not_found,
                    "snapshot expired or unknown");
    return it->second.handle;
}
Response ApiRouter::maintenance(bool compact) {
    std::lock_guard admission(admissionMutex_);
    if (stopping_)
        return errorResponse(Code::shutting_down);
    std::lock_guard registry(registryMutex_);
    while (operations_.size() >= options_.maxMaintenanceOperations) {
        auto it = std::find_if(operations_.begin(), operations_.end(), [](const auto &entry) {
            return entry.second.state == "done" || entry.second.state == "failed";
        });
        if (it == operations_.end())
            return errorResponse(Code::busy);
        operations_.erase(it);
    }
    const auto id = sessionPrefix_ + std::to_string(nextOperation_++);
    operations_.emplace(id, Operation{});
    net::post(maintenance_, [this, id, compact] {
        {
            std::lock_guard lock(registryMutex_);
            operations_.at(id).state = "running";
        }
        Status status;
        try {
            status = compact ? db_.compact() : db_.flush();
        } catch (...) {
            status = detail::exceptionStatus();
        }
        {
            std::lock_guard lock(registryMutex_);
            auto &operation = operations_.at(id);
            operation.state = status ? "done" : "failed";
            operation.result = status.code;
        }
    });
    return response(http::status::accepted, json::object{{"success", true}, {"operation_id", id}});
}
Response ApiRouter::route(const Request &request) {
    try {
        const std::string_view path(request.target().data(), request.target().size());
        const auto method = request.method();
        if (method == http::verb::get && path == "/v1/health") {
            const auto s = db_.health();
            return s ? response(http::status::ok, json::object{{"status", "ok"}})
                     : errorResponse(s.code);
        }
        if (method == http::verb::get && path == "/v1/stats")
            return response(http::status::ok, statsJSON(db_.statistics()));
        if (method == http::verb::get && path.starts_with("/v1/admin/operations/")) {
            std::lock_guard lock(registryMutex_);
            auto it = operations_.find(std::string(path.substr(21)));
            if (it == operations_.end())
                return errorResponse(Code::not_found);
            return response(http::status::ok, json::object{{"state", it->second.state},
                                                           {"code", codeName(it->second.result)}});
        }
        if (method == http::verb::delete_ && path.starts_with("/v1/snapshots/")) {
            std::lock_guard lock(registryMutex_);
            if (snapshots_.erase(std::string(path.substr(14))) == 0)
                return errorResponse(Code::not_found);
            return response(http::status::ok, json::object{{"success", true}});
        }
        if (method != http::verb::post)
            return errorResponse(Code::not_found);
        detail::require(request[http::field::content_type].starts_with("application/json"),
                        Code::invalid_argument, "JSON content type required");
        beast::error_code ec;
        const auto value = json::parse(request.body().empty() ? "{}" : request.body(), ec);
        detail::require(!ec && value.is_object(), Code::invalid_argument, "invalid JSON object");
        const auto &object = value.as_object();
        if (path == "/v1/snapshots") {
            std::lock_guard lock(registryMutex_);
            const auto now = clock_();
            std::erase_if(snapshots_, [now](const auto &e) { return e.second.expires <= now; });
            if (snapshots_.size() >= options_.maxServerSnapshots)
                return errorResponse(Code::resource_exhausted);
            auto s = db_.createSnapshot();
            if (!s)
                return errorResponse(s.status().code);
            const auto id = sessionPrefix_ + std::to_string(nextSnapshot_++);
            snapshots_.emplace(id, SnapshotEntry{s.value(), now + options_.snapshotTTL});
            return response(http::status::created,
                            json::object{{"snapshot_id", id},
                                         {"sequence", s.value().sequence()},
                                         {"ttl_seconds", options_.snapshotTTL.count()}});
        }
        if (path == "/v1/admin/flush")
            return maintenance(false);
        if (path == "/v1/admin/compact")
            return maintenance(true);
        const WriteOptions write{booleanField(object, "sync", true)};
        if (path == "/v1/kv/put" || path == "/v1/kv/delete" || path == "/v1/kv/get") {
            const auto key =
                base64Decode(stringField(object, "key_base64"), options_.storage.maxKeyBytes);
            if (path == "/v1/kv/get") {
                auto r = db_.get(key, {lookupSnapshot(object)});
                if (!r) {
                    if (r.status().code == Code::not_found)
                        return response(http::status::ok, json::object{{"found", false}});
                    return errorResponse(r.status().code);
                }
                return response(
                    http::status::ok,
                    json::object{{"found", true}, {"value_base64", base64Encode(r.value())}});
            }
            const auto s = path == "/v1/kv/delete"
                               ? db_.remove(key, write)
                               : db_.put(key,
                                         base64Decode(stringField(object, "value_base64"),
                                                      options_.storage.maxValueBytes),
                                         write);
            return s ? response(http::status::ok, json::object{{"success", true}})
                     : errorResponse(s.code);
        }
        if (path == "/v1/kv/batch") {
            const auto *array = object.if_contains("operations");
            detail::require(array && array->is_array() && !array->as_array().empty() &&
                                array->as_array().size() <= options_.storage.maxBatchOperations,
                            Code::invalid_argument, "invalid batch operations");
            WriteBatch batch;
            for (const auto &item : array->as_array()) {
                detail::require(item.is_object(), Code::invalid_argument,
                                "invalid batch operation");
                const auto &op = item.as_object();
                const auto type = stringField(op, "type");
                auto key =
                    base64Decode(stringField(op, "key_base64"), options_.storage.maxKeyBytes);
                if (type == "put")
                    batch.put(std::move(key), base64Decode(stringField(op, "value_base64"),
                                                           options_.storage.maxValueBytes));
                else if (type == "delete")
                    batch.remove(std::move(key));
                else
                    throw detail::Error(Code::invalid_argument, "unknown batch operation");
            }
            const auto s = db_.write(batch, write);
            return s ? response(http::status::ok, json::object{{"success", true}})
                     : errorResponse(s.code);
        }
        if (path == "/v1/kv/scan") {
            ScanOptions scan;
            if (object.if_contains("start_key_base64"))
                scan.start = base64Decode(stringField(object, "start_key_base64"),
                                          options_.storage.maxKeyBytes);
            if (object.if_contains("end_key_base64"))
                scan.end = base64Decode(stringField(object, "end_key_base64"),
                                        options_.storage.maxKeyBytes);
            if (object.if_contains("after_key_base64"))
                scan.after = base64Decode(stringField(object, "after_key_base64"),
                                          options_.storage.maxKeyBytes);
            scan.limit = static_cast<std::size_t>(numberField(
                object, "limit", std::min<std::size_t>(1000, options_.storage.maxScanResults)));
            scan.snapshot = lookupSnapshot(object);
            auto page = db_.scan(scan);
            if (!page)
                return errorResponse(page.status().code);
            json::array entries;
            for (const auto &e : page.value().entries)
                entries.emplace_back(json::object{{"key_base64", base64Encode(e.key)},
                                                  {"value_base64", base64Encode(e.value)}});
            json::object result{{"entries", std::move(entries)}};
            if (page.value().nextAfter)
                result["next_after_key_base64"] = base64Encode(*page.value().nextAfter);
            else
                result["next_after_key_base64"] = nullptr;
            return response(http::status::ok, std::move(result));
        }
        return errorResponse(Code::not_found);
    } catch (...) {
        return errorResponse(detail::exceptionStatus().code);
    }
}
} // namespace aegisdb::server
