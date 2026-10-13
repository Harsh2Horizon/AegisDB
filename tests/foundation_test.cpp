#include "core/codec.h"
#include "storage/bloom.h"
#include "storage/log.h"
#include "storage/memtable.h"
#include <algorithm>
#include <gtest/gtest.h>
#include <random>
#include <unistd.h>

using namespace aegisdb;
using namespace aegisdb::detail;
TEST(Codec, PortableChecksumsAndBoundaries) {
    EXPECT_EQ(crc32c("123456789"), 0xe3069283U);
    Encoder e;
    e.u32(0x12345678);
    e.u64(UINT64_MAX);
    e.string(std::string("\0\xff", 2));
    EXPECT_EQ(static_cast<unsigned char>(e.bytes()[0]), 0x78);
    Decoder d(e.bytes());
    EXPECT_EQ(d.u32(), 0x12345678U);
    EXPECT_EQ(d.u64(), UINT64_MAX);
    EXPECT_EQ(d.string(), std::string("\0\xff", 2));
    d.finish();
    EXPECT_THROW(d.u8(), Error);
    auto bytes = checkedPayload(e.bytes());
    EXPECT_EQ(verifyPayload(bytes), e.bytes());
    bytes[0] ^= 1;
    EXPECT_THROW(verifyPayload(bytes), Error);
}
TEST(Codec, RecordFormatRejectsOversizedFieldsBeforeAllocation) {
    Encoder key;
    key.u32(static_cast<std::uint32_t>(encodedKeyLimit + 1));
    Decoder malformedKey(key.bytes());
    EXPECT_THROW(decodeRecord(malformedKey), Error);
    Encoder value;
    value.string("");
    value.u64(1);
    value.u8(1);
    value.u32(static_cast<std::uint32_t>(encodedValueLimit + 1));
    Decoder malformedValue(value.bytes());
    EXPECT_THROW(decodeRecord(malformedValue), Error);
    Encoder record;
    EXPECT_THROW(encodeRecord(record, {"a", 1, Kind::deletion, "invalid"}), Error);
    EXPECT_THROW(encodeRecord(record, {"a", 0, Kind::value, "value"}), Error);
}
TEST(SkipList, RandomizedOrderAndVersions) {
    SkipList list;
    std::vector<Record> records;
    for (std::uint64_t i = 1; i <= 20000; ++i)
        records.push_back({std::to_string(i % 701), i, Kind::value, std::to_string(i)});
    records.push_back({"", 20001, Kind::value, ""});
    records.push_back({std::string("\xff\0", 2), 20002, Kind::value, "binary"});
    auto model = records;
    std::sort(model.begin(), model.end(), InternalLess{});
    std::mt19937 r(715);
    std::shuffle(records.begin(), records.end(), r);
    list.publish(list.prepare(std::move(records)));
    auto *cursor = list.begin();
    for (const auto &expected : model) {
        ASSERT_NE(cursor, nullptr);
        EXPECT_EQ(cursor->data.key, expected.key);
        EXPECT_EQ(cursor->data.sequence, expected.sequence);
        cursor = cursor->next[0];
    }
    EXPECT_EQ(cursor, nullptr);
    EXPECT_EQ(list.lowerBound("", maxSequence)->data.sequence, 20001);
    auto *n = list.lowerBound("10", 5000);
    ASSERT_NE(n, nullptr);
    EXPECT_EQ(n->data.key, "10");
    EXPECT_LE(n->data.sequence, 5000);
    EXPECT_GT(list.memoryUsage(), 0);
}
TEST(Memtable, SnapshotAndLifetime) {
    auto m = std::make_shared<Memtable>(1);
    EXPECT_FALSE(m->get("a", maxSequence));
    m->publish(m->prepare({{"a", 1, Kind::value, "old"}, {"a", 2, Kind::deletion, ""}}));
    EXPECT_EQ(m->get("a", 1)->value, "old");
    EXPECT_EQ(m->get("a", 2)->kind, Kind::deletion);
    auto it = m->iterator();
    m.reset();
    ASSERT_TRUE(it->valid());
    EXPECT_EQ(it->record().sequence, 2);
}
TEST(WAL, AtomicFramesAndStrictTail) {
    auto fs = makePosixFileSystem();
    auto path = std::filesystem::temp_directory_path() / ("aegis-wal-" + std::to_string(getpid()));
    fs->remove(path);
    auto log = FramedLog::create(*fs, path, walMagic);
    const std::vector<Record> batch{{"a", 1, Kind::value, "x"}, {"b", 2, Kind::deletion, ""}};
    log->append(encodeBatch(batch), true);
    const auto good = log->size();
    log.reset();
    auto f = fs->open(path, OpenMode::append_existing);
    f->append(std::string("\xff\0", 2));
    std::vector<Record> replayed;
    auto consume = [&](std::string_view bytes) {
        auto b = decodeBatch(bytes);
        replayed.insert(replayed.end(), b.begin(), b.end());
    };
    EXPECT_EQ(FramedLog::replay(*f, walMagic, true, consume), good);
    ASSERT_EQ(replayed.size(), 2);
    EXPECT_EQ(replayed[1].sequence, 2);
    EXPECT_THROW(FramedLog::replay(*f, walMagic, false, consume), Error);
    f->truncate(good);
    auto payload = encodeBatch(batch);
    Encoder e;
    e.u32(static_cast<std::uint32_t>(payload.size()));
    e.u32(crc32c(payload) ^ 1);
    e.u32(crc32c(e.bytes()));
    e.raw(payload);
    f->append(e.bytes());
    EXPECT_THROW(FramedLog::replay(*f, walMagic, true, consume), Error);
    f.reset();
    fs->remove(path);
}
TEST(WAL, EveryTruncationPreservesOnlyCompleteBatches) {
    auto fs = makePosixFileSystem();
    auto path =
        std::filesystem::temp_directory_path() / ("aegis-tails-" + std::to_string(getpid()));
    fs->remove(path);
    auto log = FramedLog::create(*fs, path, walMagic);
    log->append(encodeBatch({{"a", 1, Kind::value, "A"}}), true);
    const auto first = log->size();
    log->append(encodeBatch({{"b", 2, Kind::value, "B"}, {"c", 3, Kind::value, "C"}}), true);
    const auto end = log->size();
    log.reset();
    auto source = fs->open(path, OpenMode::read_only);
    const auto bytes = source->read(0, static_cast<std::size_t>(end));
    source.reset();
    for (auto length = first; length < end; ++length) {
        fs->remove(path);
        auto file = fs->open(path, OpenMode::create_exclusive);
        file->append(std::string_view(bytes).substr(0, static_cast<std::size_t>(length)));
        file.reset();
        file = fs->open(path, OpenMode::read_only);
        std::size_t replayed = 0;
        EXPECT_EQ(FramedLog::replay(
                      *file, walMagic, true,
                      [&](std::string_view payload) { replayed += decodeBatch(payload).size(); }),
                  first);
        EXPECT_EQ(replayed, 1);
    }
    fs->remove(path);
}
TEST(Bloom, NoFalseNegativesAndMeasuredFalsePositives) {
    std::vector<BloomFilter::Hash> hashes;
    for (int i = 0; i < 10000; ++i)
        hashes.push_back(BloomFilter::hash(std::to_string(i)));
    for (double rate : {0.1, 0.01, 0.001}) {
        const auto f = BloomFilter::decode(BloomFilter::build(hashes, rate).encode());
        for (int i = 0; i < 10000; ++i)
            ASSERT_TRUE(f.mayContain(std::to_string(i)));
        int positives = 0;
        for (int i = 10000; i < 30000; ++i)
            positives += f.mayContain(std::to_string(i));
        EXPECT_LT(static_cast<double>(positives) / 20000, rate * 2 + 0.005);
    }
    EXPECT_FALSE(BloomFilter::decode(BloomFilter::build({}, 0.01).encode()).mayContain(""));
}
