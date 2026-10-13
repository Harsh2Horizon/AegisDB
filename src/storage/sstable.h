#pragma once
#include "storage/bloom.h"
#include "storage/cache.h"
#include "storage/memtable.h"
#include <aegisdb/filesystem.h>

namespace aegisdb::detail {
struct TableMeta {
    std::uint64_t number{}, size{};
    std::uint32_t level{};
    std::string smallest, largest;
    Sequence minSequence{}, maxSequence{};
};
struct BlockHandle {
    std::uint64_t offset{};
    std::uint32_t length{};
};
struct IndexEntry {
    InternalKey separator;
    BlockHandle handle;
};
class DataBlockBuilder {
  public:
    void add(const Record &);
    std::string finish();
    std::size_t size() const { return data_.bytes().size() + 8; }
    bool empty() const { return count_ == 0; }

  private:
    Encoder data_;
    std::uint32_t count_{};
};
class DataBlockReader {
  public:
    static DataBlock decode(std::string_view);
};
class SSTableBuilder {
  public:
    SSTableBuilder(FileSystem &, std::filesystem::path directory, std::uint64_t number,
                   std::size_t blockBytes, double bloomRate);
    void add(const Record &);
    TableMeta finish(std::uint32_t level);
    std::size_t estimatedSize() const { return static_cast<std::size_t>(offset_) + block_.size(); }
    bool empty() const { return records_ == 0; }

  private:
    BlockHandle writeSection(std::string bytes);
    void flushBlock();
    FileSystem &fs_;
    std::filesystem::path directory_, temporary_, final_;
    std::unique_ptr<File> file_;
    std::size_t blockBytes_;
    double bloomRate_;
    std::uint64_t offset_{16}, records_{};
    TableMeta meta_;
    DataBlockBuilder block_;
    std::optional<InternalKey> last_;
    std::vector<IndexEntry> index_;
    std::vector<BloomFilter::Hash> hashes_;
};
class SSTableReader : public std::enable_shared_from_this<SSTableReader> {
  public:
    SSTableReader(FileSystem &, const std::filesystem::path &, const TableMeta &, BlockCache &,
                  Metrics &);
    std::optional<Record> get(std::string_view key, Sequence sequence) const;
    std::unique_ptr<RecordIterator> iterator(std::string_view start = {}) const;
    const TableMeta &metadata() const { return meta_; }
    void verify() const;

  private:
    class Iterator;
    std::shared_ptr<const DataBlock> readBlock(std::size_t index) const;
    std::size_t selectBlock(std::string_view key, Sequence sequence) const;
    std::unique_ptr<File> file_;
    TableMeta meta_;
    BlockCache &cache_;
    Metrics &metrics_;
    BloomFilter bloom_;
    std::vector<IndexEntry> index_;
};
void encodeTableMeta(Encoder &, const TableMeta &);
TableMeta decodeTableMeta(Decoder &);
} // namespace aegisdb::detail
