#include "storage/sstable.h"
#include "storage/log.h"
#include <algorithm>

namespace aegisdb::detail {
namespace {
constexpr std::size_t footerSize = 52;
constexpr std::string_view tableMagic = "AEGSST01", footerMagic = "AEGEND01";
void encodeHandle(Encoder &e, const BlockHandle &h) {
    e.u64(h.offset);
    e.u32(h.length);
}
BlockHandle decodeHandle(Decoder &d) { return {d.u64(), d.u32()}; }
std::string readSection(const File &file, BlockHandle h, std::uint64_t limit) {
    require(h.offset >= 16 && h.offset <= limit && h.length >= 4 && h.length <= maxFrameBytes &&
                h.length <= limit - h.offset,
            Code::corruption, "invalid SSTable block handle");
    auto bytes = file.read(h.offset, h.length);
    require(bytes.size() == h.length, Code::corruption, "truncated SSTable section");
    return bytes;
}
} // namespace
void encodeTableMeta(Encoder &e, const TableMeta &m) {
    e.u64(m.number);
    e.u64(m.size);
    e.u32(m.level);
    e.string(m.smallest);
    e.string(m.largest);
    e.u64(m.minSequence);
    e.u64(m.maxSequence);
}
TableMeta decodeTableMeta(Decoder &d) {
    TableMeta m;
    m.number = d.u64();
    m.size = d.u64();
    m.level = d.u32();
    m.smallest = d.string(encodedKeyLimit);
    m.largest = d.string(encodedKeyLimit);
    m.minSequence = d.u64();
    m.maxSequence = d.u64();
    require(m.number > 0 && m.smallest <= m.largest && m.minSequence <= m.maxSequence,
            Code::corruption, "invalid table metadata");
    return m;
}
void DataBlockBuilder::add(const Record &r) {
    require(count_ < UINT32_MAX, Code::resource_exhausted, "block record count overflow");
    encodeRecord(data_, r);
    ++count_;
}
std::string DataBlockBuilder::finish() {
    Encoder e;
    e.u32(count_);
    e.raw(data_.bytes());
    data_ = Encoder{};
    count_ = 0;
    return checkedPayload(e.bytes());
}
DataBlock DataBlockReader::decode(std::string_view bytes) {
    Decoder d(verifyPayload(bytes));
    const auto count = d.u32();
    require(count > 0 && count <= d.remaining() / 17, Code::corruption,
            "invalid block record count");
    DataBlock records;
    records.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto r = decodeRecord(d);
        require(records.empty() || InternalLess{}(records.back(), r), Code::corruption,
                "unordered data block");
        records.push_back(std::move(r));
    }
    d.finish();
    return records;
}
SSTableBuilder::SSTableBuilder(FileSystem &fs, std::filesystem::path directory,
                               std::uint64_t number, std::size_t blockBytes, double bloomRate)
    : fs_(fs), directory_(std::move(directory)),
      temporary_(directory_ / fileName(number, ".sst.tmp")),
      final_(directory_ / fileName(number, ".sst")),
      file_(fs.open(temporary_, OpenMode::create_exclusive)), blockBytes_(blockBytes),
      bloomRate_(bloomRate) {
    require(blockBytes > 0 && blockBytes <= maxFrameBytes / 2, Code::invalid_argument,
            "invalid block target");
    meta_.number = number;
    meta_.minSequence = maxSequence;
    Encoder e;
    e.raw(tableMagic);
    e.u32(1);
    file_->append(checkedPayload(e.bytes()));
}
BlockHandle SSTableBuilder::writeSection(std::string bytes) {
    require(bytes.size() <= maxFrameBytes, Code::resource_exhausted, "SSTable section too large");
    BlockHandle h{offset_, static_cast<std::uint32_t>(bytes.size())};
    file_->append(bytes);
    offset_ += bytes.size();
    return h;
}
void SSTableBuilder::flushBlock() {
    if (block_.empty())
        return;
    const auto h = writeSection(block_.finish());
    index_.push_back({*last_, h});
}
void SSTableBuilder::add(const Record &r) {
    require(r.sequence > 0 &&
                (!last_ || compareInternal(last_->key, last_->sequence, r.key, r.sequence) < 0),
            Code::invalid_argument, "SSTable input out of order");
    require(recordBytes(r) <= maxFrameBytes - 8, Code::invalid_argument,
            "record exceeds block limit");
    if (!block_.empty() && block_.size() + recordBytes(r) > blockBytes_)
        flushBlock();
    if (!last_ || last_->key != r.key)
        hashes_.push_back(BloomFilter::hash(r.key));
    if (records_ == 0)
        meta_.smallest = r.key;
    meta_.largest = r.key;
    meta_.minSequence = std::min(meta_.minSequence, r.sequence);
    meta_.maxSequence = std::max(meta_.maxSequence, r.sequence);
    block_.add(r);
    last_ = InternalKey{r.key, r.sequence};
    ++records_;
}
TableMeta SSTableBuilder::finish(std::uint32_t level) {
    flushBlock();
    if (records_ == 0)
        meta_.minSequence = 0;
    meta_.level = level;
    const auto bloom =
        writeSection(checkedPayload(BloomFilter::build(hashes_, bloomRate_).encode()));
    Encoder props;
    props.u64(meta_.number);
    props.string(meta_.smallest);
    props.string(meta_.largest);
    props.u64(meta_.minSequence);
    props.u64(meta_.maxSequence);
    props.u64(records_);
    const auto properties = writeSection(checkedPayload(props.bytes()));
    Encoder idx;
    require(index_.size() <= UINT32_MAX, Code::resource_exhausted, "too many blocks");
    idx.u32(static_cast<std::uint32_t>(index_.size()));
    for (const auto &entry : index_) {
        idx.string(entry.separator.key);
        idx.u64(entry.separator.sequence);
        encodeHandle(idx, entry.handle);
    }
    const auto index = writeSection(checkedPayload(idx.bytes()));
    Encoder footer;
    footer.raw(footerMagic);
    footer.u32(1);
    encodeHandle(footer, bloom);
    encodeHandle(footer, properties);
    encodeHandle(footer, index);
    file_->append(checkedPayload(footer.bytes()));
    file_->sync();
    meta_.size = offset_ + footerSize;
    file_.reset();
    fs_.rename(temporary_, final_);
    fs_.syncDirectory(directory_);
    return meta_;
}
SSTableReader::SSTableReader(FileSystem &fs, const std::filesystem::path &path,
                             const TableMeta &meta, BlockCache &cache, Metrics &metrics)
    : file_(fs.open(path, OpenMode::read_only)), meta_(meta), cache_(cache), metrics_(metrics) {
    const auto size = file_->size();
    require(size == meta.size && size >= 16 + footerSize, Code::corruption,
            "SSTable size mismatch");
    const auto header = file_->read(0, 16);
    Decoder h(verifyPayload(header));
    require(h.raw(8) == tableMagic && h.u32() == 1, Code::corruption, "unsupported table header");
    h.finish();
    const auto footer = file_->read(size - footerSize, footerSize);
    Decoder f(verifyPayload(footer));
    require(f.raw(8) == footerMagic && f.u32() == 1, Code::corruption, "unsupported footer");
    const auto bh = decodeHandle(f), ph = decodeHandle(f), ih = decodeHandle(f);
    f.finish();
    require(bh.offset <= size && bh.length <= size - bh.offset &&
                bh.offset + bh.length == ph.offset && ph.offset <= size &&
                ph.length <= size - ph.offset && ph.offset + ph.length == ih.offset &&
                ih.offset <= size && ih.length <= size - ih.offset &&
                ih.offset + ih.length == size - footerSize,
            Code::corruption, "invalid metadata layout");
    const auto b = readSection(*file_, bh, size - footerSize);
    bloom_ = BloomFilter::decode(verifyPayload(b));
    const auto p = readSection(*file_, ph, size - footerSize);
    Decoder props(verifyPayload(p));
    const auto number = props.u64();
    const auto smallest = props.string(encodedKeyLimit), largest = props.string(encodedKeyLimit);
    const auto min = props.u64(), max = props.u64(), count = props.u64();
    props.finish();
    require(number == meta.number && smallest == meta.smallest && largest == meta.largest &&
                min == meta.minSequence && max == meta.maxSequence,
            Code::corruption, "manifest/table metadata mismatch");
    const auto i = readSection(*file_, ih, size - footerSize);
    Decoder idx(verifyPayload(i));
    const auto blocks = idx.u32();
    require(blocks <= idx.remaining() / 24, Code::corruption, "invalid index count");
    index_.reserve(blocks);
    std::uint64_t next = 16;
    for (std::uint32_t n = 0; n < blocks; ++n) {
        IndexEntry entry;
        entry.separator.key = idx.string(encodedKeyLimit);
        entry.separator.sequence = idx.u64();
        entry.handle = decodeHandle(idx);
        require(entry.handle.offset == next && entry.handle.length >= 8 &&
                    entry.handle.length <= maxFrameBytes && next <= bh.offset &&
                    entry.handle.length <= bh.offset - next,
                Code::corruption, "invalid sparse block coverage");
        require(index_.empty() || InternalLess{}(index_.back().separator, entry.separator),
                Code::corruption, "unordered sparse index");
        next += entry.handle.length;
        index_.push_back(std::move(entry));
    }
    idx.finish();
    require(next == bh.offset && (count == 0) == index_.empty(), Code::corruption,
            "sparse index coverage mismatch");
    if (!index_.empty())
        require(index_.back().separator.key == meta.largest && min > 0 && bloom_.bitCount() > 0,
                Code::corruption, "invalid table bounds");
}
std::size_t SSTableReader::selectBlock(std::string_view key, Sequence sequence) const {
    const auto it =
        std::lower_bound(index_.begin(), index_.end(), std::pair{key, sequence},
                         [](const IndexEntry &e, const auto &target) {
                             return compareInternal(e.separator.key, e.separator.sequence,
                                                    target.first, target.second) < 0;
                         });
    return static_cast<std::size_t>(it - index_.begin());
}
std::shared_ptr<const DataBlock> SSTableReader::readBlock(std::size_t index) const {
    const auto &entry = index_.at(index);
    if (auto block = cache_.get(meta_.number, entry.handle.offset))
        return block;
    auto bytes = file_->read(entry.handle.offset, entry.handle.length);
    require(bytes.size() == entry.handle.length, Code::corruption, "truncated data block");
    auto block = std::make_shared<DataBlock>(DataBlockReader::decode(bytes));
    const auto &last = block->back();
    require(compareInternal(last.key, last.sequence, entry.separator.key,
                            entry.separator.sequence) == 0,
            Code::corruption, "sparse separator mismatch");
    if (index > 0)
        require(compareInternal(index_[index - 1].separator.key,
                                index_[index - 1].separator.sequence, block->front().key,
                                block->front().sequence) < 0,
                Code::corruption, "cross-block ordering violation");
    for (const auto &r : *block)
        require(r.key >= meta_.smallest && r.key <= meta_.largest &&
                    r.sequence >= meta_.minSequence && r.sequence <= meta_.maxSequence &&
                    bloom_.mayContain(r.key),
                Code::corruption, "record violates table metadata");
    std::size_t charge = sizeof(DataBlock) + block->capacity() * sizeof(Record);
    for (const auto &r : *block)
        charge += r.key.capacity() + r.value.capacity();
    cache_.insert(meta_.number, entry.handle.offset, block, charge);
    return block;
}
std::optional<Record> SSTableReader::get(std::string_view key, Sequence sequence) const {
    if (key < meta_.smallest || key > meta_.largest || sequence < meta_.minSequence)
        return {};
    ++metrics_.bloomChecks;
    if (!bloom_.mayContain(key)) {
        ++metrics_.bloomNegatives;
        return {};
    }
    const auto index = selectBlock(key, sequence);
    if (index == index_.size())
        return {};
    const auto block = readBlock(index);
    const auto it = std::lower_bound(block->begin(), block->end(), std::pair{key, sequence},
                                     [](const Record &r, const auto &target) {
                                         return compareInternal(r.key, r.sequence, target.first,
                                                                target.second) < 0;
                                     });
    if (it != block->end() && it->key == key)
        return *it;
    return {};
}
class SSTableReader::Iterator final : public RecordIterator {
  public:
    Iterator(std::shared_ptr<const SSTableReader> owner, std::string_view start)
        : owner_(std::move(owner)), index_(owner_->selectBlock(start, maxSequence)) {
        if (index_ < owner_->index_.size()) {
            block_ = owner_->readBlock(index_);
            position_ = static_cast<std::size_t>(
                std::lower_bound(
                    block_->begin(), block_->end(), start,
                    [](const Record &r, std::string_view key) { return r.key < key; }) -
                block_->begin());
            if (position_ == block_->size())
                advance();
        }
    }
    bool valid() const override { return block_ && position_ < block_->size(); }
    const Record &record() const override { return (*block_)[position_]; }
    void next() override {
        if (++position_ == block_->size())
            advance();
    }

  private:
    void advance() {
        position_ = 0;
        if (++index_ < owner_->index_.size())
            block_ = owner_->readBlock(index_);
        else
            block_.reset();
    }
    std::shared_ptr<const SSTableReader> owner_;
    std::size_t index_{}, position_{};
    std::shared_ptr<const DataBlock> block_;
};
std::unique_ptr<RecordIterator> SSTableReader::iterator(std::string_view start) const {
    return std::make_unique<Iterator>(shared_from_this(), start);
}
void SSTableReader::verify() const {
    for (std::size_t i = 0; i < index_.size(); ++i)
        (void)readBlock(i);
}
} // namespace aegisdb::detail
