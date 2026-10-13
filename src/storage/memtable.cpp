#include "storage/memtable.h"
#include <mutex>
#include <optional>

namespace aegisdb::detail {
SkipList::SkipList(std::size_t maxHeight, double probability, std::uint64_t seed)
    : random_(seed), maxHeight_(maxHeight), promote_(probability) {
    require(maxHeight > 0 && maxHeight <= heightLimit && probability > 0 && probability < 1,
            Code::invalid_argument, "invalid skip-list parameters");
}
SkipList::Prepared SkipList::prepare(std::vector<Record> records) {
    Prepared result;
    result.reserve(records.size());
    require(records.size() <= nodes_.max_size() - nodes_.size(), Code::resource_exhausted,
            "memtable node limit");
    const auto required = nodes_.size() + records.size();
    if (required > nodes_.capacity()) {
        const auto grown = nodes_.capacity() > nodes_.max_size() / 2
                               ? nodes_.max_size()
                               : std::max<std::size_t>(64, nodes_.capacity() * 2);
        nodes_.reserve(std::max(required, grown));
    }
    for (auto &r : records) {
        std::size_t h = 1;
        while (h < maxHeight_ && promote_(random_))
            ++h;
        result.push_back(std::make_unique<Node>(std::move(r), h));
    }
    return result;
}
void SkipList::publish(Prepared &&nodes) noexcept {
    for (auto &owning : nodes) {
        auto *node = owning.get();
        std::array<Node **, heightLimit> previous{};
        Node *cursor = nullptr;
        for (std::size_t level = maxHeight_; level-- > 0;) {
            auto *next = cursor ? cursor->next[level] : head_[level];
            while (next && InternalLess{}(next->data, node->data)) {
                cursor = next;
                next = cursor->next[level];
            }
            previous[level] = cursor ? &cursor->next[level] : &head_[level];
        }
        for (std::size_t level = 0; level < node->next.size(); ++level) {
            node->next[level] = *previous[level];
            *previous[level] = node;
        }
        bytes_ += sizeof(Node) + node->next.capacity() * sizeof(Node *) +
                  node->data.key.capacity() + node->data.value.capacity() +
                  sizeof(std::unique_ptr<Node>);
        nodes_.push_back(std::move(owning));
    }
}
const SkipList::Node *SkipList::lowerBound(std::string_view key, Sequence sequence) const {
    const Node *cursor = nullptr;
    for (std::size_t level = maxHeight_; level-- > 0;) {
        auto *next = cursor ? cursor->next[level] : head_[level];
        while (next && compareInternal(next->data.key, next->data.sequence, key, sequence) < 0) {
            cursor = next;
            next = cursor->next[level];
        }
    }
    return cursor ? cursor->next[0] : head_[0];
}
SkipList::Prepared Memtable::prepare(std::vector<Record> records) {
    std::unique_lock lock(mutex_);
    return list_.prepare(std::move(records));
}
void Memtable::publish(SkipList::Prepared &&nodes) {
    std::unique_lock lock(mutex_);
    list_.publish(std::move(nodes));
}
std::optional<Record> Memtable::get(std::string_view key, Sequence sequence) const {
    std::shared_lock lock(mutex_);
    auto *n = list_.lowerBound(key, sequence);
    if (n && n->data.key == key)
        return n->data;
    return std::nullopt;
}
std::size_t Memtable::memoryUsage() const {
    std::shared_lock lock(mutex_);
    return list_.memoryUsage();
}
bool Memtable::empty() const {
    std::shared_lock lock(mutex_);
    return list_.size() == 0;
}
class Memtable::Iterator final : public RecordIterator {
  public:
    Iterator(std::shared_ptr<const Memtable> owner, std::string_view start)
        : owner_(std::move(owner)), lock_(owner_->mutex_),
          node_(owner_->list_.lowerBound(start, maxSequence)) {}
    bool valid() const override { return node_ != nullptr; }
    const Record &record() const override { return node_->data; }
    void next() override { node_ = node_->next[0]; }

  private:
    std::shared_ptr<const Memtable> owner_;
    std::shared_lock<std::shared_mutex> lock_;
    const SkipList::Node *node_;
};
std::unique_ptr<RecordIterator> Memtable::iterator(std::string_view start) const {
    return std::make_unique<Iterator>(shared_from_this(), start);
}
} // namespace aegisdb::detail
