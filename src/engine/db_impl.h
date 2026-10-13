#pragma once
#include "engine/compaction.h"
#include <aegisdb/db.h>
#include <condition_variable>
#include <deque>
#include <map>
#include <thread>

namespace aegisdb::detail {
struct SnapshotState {
    std::mutex mutex;
    std::map<Sequence, std::size_t> active;
    std::size_t handles{};
    Sequence oldest(Sequence fallback) {
        std::lock_guard lock(mutex);
        return active.empty() ? fallback : active.begin()->first;
    }
};
struct SnapshotToken {
    SnapshotToken(const SnapshotToken &) = delete;
    SnapshotToken &operator=(const SnapshotToken &) = delete;
    SnapshotToken(SnapshotToken &&) = delete;
    SnapshotToken &operator=(SnapshotToken &&) = delete;
    std::shared_ptr<SnapshotState> owner;
    Sequence sequence;
    SnapshotToken(std::shared_ptr<SnapshotState> o, Sequence s)
        : owner(std::move(o)), sequence(s) {}
    ~SnapshotToken() {
        std::lock_guard lock(owner->mutex);
        auto it = owner->active.find(sequence);
        if (it != owner->active.end() && --it->second == 0)
            owner->active.erase(it);
        --owner->handles;
    }
};
class DBImpl {
  public:
    DBImpl(std::filesystem::path, Options);
    ~DBImpl();
    Status write(const WriteBatch &, WriteOptions);
    Result<std::string> get(std::string_view, ReadOptions) const;
    Result<ScanPage> scan(const ScanOptions &) const;
    Result<Snapshot> snapshot();
    Status flush();
    Status compact();
    Statistics statistics() const;
    Status health() const;

  private:
    struct ReadView {
        Sequence sequence;
        std::shared_ptr<Memtable> active;
        std::deque<std::shared_ptr<Memtable>> immutable;
        std::shared_ptr<const Version> version;
    };
    ReadView readView(const Snapshot &) const;
    void recover();
    void validateOptions() const;
    void rotate();
    void waitForRoom(std::unique_lock<std::mutex> &);
    void flushOne();
    bool compactOne(std::optional<std::size_t> forced = {});
    void background(std::stop_token);
    void fail(Status) const;
    std::uint64_t allocate();
    void reclaim();
    std::shared_ptr<SSTableReader> openTable(const TableMeta &);
    std::filesystem::path path_;
    Options options_;
    std::shared_ptr<FileSystem> fs_;
    std::unique_ptr<DirectoryLock> directoryLock_;
    mutable Metrics metrics_;
    mutable BlockCache cache_;
    std::unique_ptr<Manifest> manifest_;
    std::mutex manifestMutex_;
    mutable std::mutex stateMutex_;
    mutable std::condition_variable stateCV_;
    std::timed_mutex writeMutex_;
    std::mutex maintenanceMutex_;
    Sequence committed_{};
    mutable Status error_;
    bool stopping_{}, workRequested_{};
    std::shared_ptr<Memtable> active_;
    std::deque<std::shared_ptr<Memtable>> immutable_;
    std::shared_ptr<const Version> version_;
    std::unique_ptr<FramedLog> wal_;
    std::shared_ptr<SnapshotState> snapshots_{std::make_shared<SnapshotState>()};
    std::vector<std::pair<std::weak_ptr<SSTableReader>, std::filesystem::path>> obsolete_;
    std::jthread worker_;
};
} // namespace aegisdb::detail
