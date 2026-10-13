#pragma once
#include "core/record.h"
#include <array>
#include <memory>
#include <optional>
#include <random>
#include <shared_mutex>
#include <vector>

namespace aegisdb::detail {
class RecordIterator {
  public:
    virtual ~RecordIterator() = default;
    virtual bool valid() const = 0;
    virtual const Record &record() const = 0;
    virtual void next() = 0;
};
// External synchronization protects links. Node addresses never change; unique_ptrs own nodes.
class SkipList {
  public:
    static constexpr std::size_t heightLimit = 32;
    struct Node {
        Record data;
        std::vector<Node *> next;
        Node(Record r, std::size_t h) : data(std::move(r)), next(h, nullptr) {}
    };
    using Prepared = std::vector<std::unique_ptr<Node>>;
    explicit SkipList(std::size_t maxHeight = 16, double probability = 0.25,
                      std::uint64_t seed = 0xAE615DB);
    SkipList(const SkipList &) = delete;
    SkipList &operator=(const SkipList &) = delete;
    SkipList(SkipList &&) = delete;
    SkipList &operator=(SkipList &&) = delete;
    // One serialized writer keeps at most one outstanding Prepared batch for reserved publication.
    Prepared prepare(std::vector<Record> records);
    void publish(Prepared &&nodes) noexcept;
    const Node *lowerBound(std::string_view key, Sequence sequence) const;
    const Node *begin() const { return head_[0]; }
    std::size_t memoryUsage() const {
        return bytes_ + (nodes_.capacity() - nodes_.size()) * sizeof(std::unique_ptr<Node>);
    }
    std::size_t size() const { return nodes_.size(); }

  private:
    std::array<Node *, heightLimit> head_{};
    std::vector<std::unique_ptr<Node>> nodes_;
    std::mt19937_64 random_;
    std::size_t maxHeight_;
    std::bernoulli_distribution promote_;
    std::size_t bytes_{};
};
class Memtable : public std::enable_shared_from_this<Memtable> {
  public:
    explicit Memtable(std::uint64_t wal, std::size_t maxHeight = 16, double probability = 0.25)
        : walNumber(wal), list_(maxHeight, probability) {}
    SkipList::Prepared prepare(std::vector<Record>);
    void publish(SkipList::Prepared &&);
    std::optional<Record> get(std::string_view key, Sequence sequence) const;
    std::unique_ptr<RecordIterator> iterator(std::string_view start = {}) const;
    std::size_t memoryUsage() const;
    bool empty() const;
    const std::uint64_t walNumber;

  private:
    class Iterator;
    mutable std::shared_mutex mutex_;
    SkipList list_;
};
} // namespace aegisdb::detail
