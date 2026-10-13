#include "storage/cache.h"

namespace aegisdb::detail {
std::shared_ptr<const DataBlock> BlockCache::get(std::uint64_t file, std::uint64_t offset) {
    std::lock_guard lock(mutex_);
    const auto it = map_.find({file, offset});
    if (it == map_.end()) {
        ++metrics_.cacheMisses;
        return {};
    }
    lru_.splice(lru_.begin(), lru_, it->second);
    ++metrics_.cacheHits;
    return it->second->block;
}
void BlockCache::insert(std::uint64_t file, std::uint64_t offset,
                        std::shared_ptr<const DataBlock> block, std::size_t charge) {
    if (charge > capacity_)
        return;
    std::lock_guard lock(mutex_);
    const Key key{file, offset};
    if (map_.contains(key))
        return;
    lru_.push_front({key, std::move(block), charge});
    try {
        map_.emplace(key, lru_.begin());
    } catch (...) {
        lru_.pop_front();
        throw;
    }
    usage_ += charge;
    while (usage_ > capacity_) {
        auto it = std::prev(lru_.end());
        usage_ -= it->charge;
        map_.erase(it->key);
        lru_.erase(it);
    }
}
} // namespace aegisdb::detail
