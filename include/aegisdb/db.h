#pragma once
#include <aegisdb/options.h>
#include <aegisdb/status.h>
#include <optional>
#include <string>
#include <vector>


namespace aegisdb {

namespace detail {

class DBImpl;
struct SnapshotToken;

} // namespace detail

class Snapshot {
  public:
    Snapshot() = default;
    std::uint64_t sequence() const;
    explicit operator bool() const { return bool(token_); }

  private:
    explicit Snapshot(std::shared_ptr<detail::SnapshotToken> token) : token_(std::move(token)) {}
    std::shared_ptr<detail::SnapshotToken> token_;
    friend class detail::DBImpl;
};
struct ReadOptions {
    Snapshot snapshot;
};
struct KeyValue {
    std::string key, value;
    bool operator==(const KeyValue &) const = default;
};
struct Mutation {
    enum class Type { put, remove };
    Type type;
    std::string key, value;
};
class WriteBatch {
  public:
    WriteBatch &put(std::string key, std::string value) {
        operations_.push_back({Mutation::Type::put, std::move(key), std::move(value)});
        return *this;
    }
    WriteBatch &remove(std::string key) {
        operations_.push_back({Mutation::Type::remove, std::move(key), {}});
        return *this;
    }
    const std::vector<Mutation> &operations() const { return operations_; }

  private:
    std::vector<Mutation> operations_;
};
struct ScanOptions {
    std::string start;
    std::optional<std::string> end;
    std::optional<std::string>
        after; // Exclusive continuation key; use the same Snapshot for every page.
    std::size_t limit{1000};
    Snapshot snapshot;
};
struct ScanPage {
    std::vector<KeyValue> entries;
    std::optional<std::string> nextAfter;
};
struct LevelStats {
    std::uint64_t tables{}, bytes{};
};
struct Statistics {
    std::uint64_t puts{}, gets{}, deletes{}, scans{}, readHits{}, readMisses{}, walBytes{},
        walSyncs{}, memtableBytes{}, immutableMemtables{}, bloomChecks{}, bloomNegatives{},
        cacheHits{}, cacheMisses{}, cacheBytes{}, flushes{}, flushBytesWritten{}, compactions{},
        compactionBytesRead{}, compactionBytesWritten{}, backgroundErrors{}, lastSequence{};
    std::vector<LevelStats> levels;
};
// Destroy only after all API calls have returned. Snapshot handles may outlive the Database.
class Database {
  public:
    static Result<std::unique_ptr<Database>> open(const std::filesystem::path &, Options = {});
    ~Database();
    Database(const Database &) = delete;
    Database &operator=(const Database &) = delete;
    Status put(std::string_view key, std::string_view value, WriteOptions = {});
    Status remove(std::string_view key, WriteOptions = {});
    Status write(const WriteBatch &, WriteOptions = {});
    Result<std::string> get(std::string_view key, ReadOptions = {}) const;
    Result<ScanPage> scan(const ScanOptions &) const;
    Result<Snapshot> createSnapshot();
    Status flush();   // Waits for all records present at call entry to be installed durably.
    Status compact(); // Flush then perform one deterministic sweep through all compactable levels.
    Statistics statistics() const;
    Status health() const;

  private:
    explicit Database(std::unique_ptr<detail::DBImpl>);
    std::unique_ptr<detail::DBImpl> impl_;
};
} // namespace aegisdb
