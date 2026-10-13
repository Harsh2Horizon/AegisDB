#pragma once
#include "storage/memtable.h"
#include "storage/sstable.h"
#include <algorithm>
#include <queue>

namespace aegisdb::detail {
// Disjoint files in one deeper level form a single ordered source. Only the current table's
// iterator reads data; references to the remaining tables pin metadata without loading blocks.
class TableSequenceIterator final : public RecordIterator {
  public:
    explicit TableSequenceIterator(std::vector<std::shared_ptr<SSTableReader>> tables,
                                   std::string_view start = {})
        : tables_(std::move(tables)) {
        index_ =
            static_cast<std::size_t>(std::lower_bound(tables_.begin(), tables_.end(), start,
                                                      [](const auto &table, std::string_view key) {
                                                          return table->metadata().largest < key;
                                                      }) -
                                     tables_.begin());
        openCurrent(start);
    }
    bool valid() const override { return current_ && current_->valid(); }
    const Record &record() const override { return current_->record(); }
    void next() override {
        current_->next();
        if (!current_->valid()) {
            ++index_;
            openCurrent({});
        }
    }

  private:
    void openCurrent(std::string_view start) {
        current_.reset();
        while (index_ < tables_.size()) {
            current_ = tables_[index_]->iterator(start);
            if (current_->valid())
                return;
            ++index_;
        }
        current_.reset();
    }
    std::vector<std::shared_ptr<SSTableReader>> tables_;
    std::size_t index_{};
    std::unique_ptr<RecordIterator> current_;
};
class MergingIterator final : public RecordIterator {
  public:
    // The heap comparator refers to this object's sources_; its address must remain stable.
    MergingIterator(const MergingIterator &) = delete;
    MergingIterator &operator=(const MergingIterator &) = delete;
    MergingIterator(MergingIterator &&) = delete;
    MergingIterator &operator=(MergingIterator &&) = delete;
    explicit MergingIterator(std::vector<std::unique_ptr<RecordIterator>> sources)
        : sources_(std::move(sources)), heap_(Compare{&sources_}) {
        for (std::size_t i = 0; i < sources_.size(); ++i)
            if (sources_[i]->valid())
                heap_.push(i);
    }
    bool valid() const override { return !heap_.empty(); }
    const Record &record() const override { return sources_[heap_.top()]->record(); }
    void next() override {
        const auto i = heap_.top();
        heap_.pop();
        sources_[i]->next();
        if (sources_[i]->valid())
            heap_.push(i);
    }

  private:
    struct Compare {
        const std::vector<std::unique_ptr<RecordIterator>> *sources;
        bool operator()(std::size_t a, std::size_t b) const {
            const auto &ar = (*sources)[a]->record();
            const auto &br = (*sources)[b]->record();
            const auto c = compareInternal(ar.key, ar.sequence, br.key, br.sequence);
            return c > 0 || (c == 0 && a > b);
        }
    };
    std::vector<std::unique_ptr<RecordIterator>> sources_;
    std::priority_queue<std::size_t, std::vector<std::size_t>, Compare> heap_;
};
} // namespace aegisdb::detail
