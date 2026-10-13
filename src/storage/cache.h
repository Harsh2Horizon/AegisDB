#pragma once
#include "core/record.h"
#include <atomic>
#include <list>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace aegisdb::detail {
struct Metrics {
    std::atomic<std::uint64_t> puts{}, gets{}, deletes{}, scans{}, hits{}, misses{}, walBytes{},
        walSyncs{}, bloomChecks{}, bloomNegatives{}, cacheHits{}, cacheMisses{}, flushes{},
        compactions{}, compactionRead{}, compactionWritten{}, flushBytes{}, backgroundErrors{};
};
using DataBlock = std::vector<Record>;
class BlockCache {
  public:
    BlockCache(std::size_t capacity, Metrics &metrics) : capacity_(capacity), metrics_(metrics) {}
    std::shared_ptr<const DataBlock> get(std::uint64_t file, std::uint64_t offset);
    void insert(std::uint64_t file, std::uint64_t offset, std::shared_ptr<const DataBlock>,
                std::size_t charge);
    std::size_t usage() const {
        std::lock_guard lock(mutex_);
        return usage_;
    }

  private:
    struct Key {
        std::uint64_t file, offset;
        bool operator==(const Key &) const = default;
    };
    struct Hash {
        std::size_t operator()(const Key &k) const {
            return static_cast<std::size_t>(
                k.file ^ (k.offset + 0x9e3779b97f4a7c15ULL + (k.file << 6) + (k.file >> 2)));
        }
    };
    struct Entry {
        Key key;
        std::shared_ptr<const DataBlock> block;
        std::size_t charge;
    };
    mutable std::mutex mutex_;
    std::size_t capacity_, usage_{};
    Metrics &metrics_;
    std::list<Entry> lru_;
    std::unordered_map<Key, std::list<Entry>::iterator, Hash> map_;
};
} // namespace aegisdb::detail
