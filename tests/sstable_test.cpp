#include "engine/merge.h"
#include "storage/sstable.h"
#include <gtest/gtest.h>
#include <unistd.h>
using namespace aegisdb;
using namespace aegisdb::detail;
TEST(SSTable, VersionsAcrossBlocksAndReopen) {
    auto fs = makePosixFileSystem();
    auto dir = std::filesystem::temp_directory_path() / ("aegis-sst-" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    fs->createDirectories(dir);
    Metrics metrics;
    BlockCache cache(1024 * 1024, metrics);
    SSTableBuilder builder(*fs, dir, 1, 128, 0.01);
    std::vector<Record> model;
    for (auto key : {"", "a", "b", "z"})
        for (std::uint64_t sequence = 100; sequence > 0; --sequence) {
            Record r{key, sequence, sequence % 7 == 0 ? Kind::deletion : Kind::value,
                     sequence % 7 == 0 ? "" : std::string(30, 'x')};
            builder.add(r);
            model.push_back(r);
        }
    const auto meta = builder.finish(0);
    auto table =
        std::make_shared<SSTableReader>(*fs, dir / fileName(1, ".sst"), meta, cache, metrics);
    table->verify();
    for (std::uint64_t s = 1; s <= 100; ++s) {
        auto r = table->get("a", s);
        ASSERT_TRUE(r);
        EXPECT_EQ(r->sequence, s);
        EXPECT_EQ(r->kind, s % 7 == 0 ? Kind::deletion : Kind::value);
    }
    EXPECT_FALSE(table->get("a", 0));
    EXPECT_FALSE(table->get("c", 100));
    auto it = table->iterator();
    for (const auto &r : model) {
        ASSERT_TRUE(it->valid());
        EXPECT_EQ(it->record().key, r.key);
        EXPECT_EQ(it->record().sequence, r.sequence);
        it->next();
    }
    EXPECT_FALSE(it->valid());
    table.reset();
    it.reset();
    BlockCache fresh(0, metrics);
    table = std::make_shared<SSTableReader>(*fs, dir / fileName(1, ".sst"), meta, fresh, metrics);
    EXPECT_EQ(table->get("b", 1)->sequence, 1);
    table.reset();
    std::filesystem::remove_all(dir);
}
TEST(SSTable, ChecksummedMetadataAndEmptyTable) {
    auto fs = makePosixFileSystem();
    auto dir = std::filesystem::temp_directory_path() / ("aegis-empty-" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    fs->createDirectories(dir);
    Metrics m;
    BlockCache cache(0, m);
    SSTableBuilder b(*fs, dir, 1, 128, 0.01);
    const auto meta = b.finish(0);
    auto table = std::make_shared<SSTableReader>(*fs, dir / fileName(1, ".sst"), meta, cache, m);
    EXPECT_FALSE(table->iterator()->valid());
    EXPECT_FALSE(table->get("", 1));
    table.reset();
    auto f = fs->open(dir / fileName(1, ".sst"), OpenMode::append_existing);
    f->truncate(meta.size - 1);
    f.reset();
    EXPECT_THROW(SSTableReader(*fs, dir / fileName(1, ".sst"), meta, cache, m), Error);
    std::filesystem::remove_all(dir);
}
TEST(SSTable, EverySectionChecksumRejectsCorruption) {
    auto fs = makePosixFileSystem();
    auto dir =
        std::filesystem::temp_directory_path() / ("aegis-sections-" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    fs->createDirectories(dir);
    SSTableBuilder builder(*fs, dir, 1, 128, 0.01);
    builder.add({"a", 2, Kind::value, "A"});
    builder.add({"b", 1, Kind::value, "B"});
    const auto meta = builder.finish(0);
    const auto path = dir / fileName(1, ".sst");
    auto file = fs->open(path, OpenMode::read_only);
    const auto original = file->read(0, static_cast<std::size_t>(meta.size));
    file.reset();
    Decoder footer(verifyPayload(std::string_view(original).substr(original.size() - 52)));
    (void)footer.raw(8);
    (void)footer.u32();
    std::vector<std::size_t> offsets{0, 16, original.size() - 1};
    for (int i = 0; i < 3; ++i) {
        offsets.push_back(static_cast<std::size_t>(footer.u64()));
        (void)footer.u32();
    }
    for (auto offset : offsets) {
        SCOPED_TRACE(offset);
        auto damaged = original;
        damaged[offset] ^= 1;
        fs->remove(path);
        auto writer = fs->open(path, OpenMode::create_exclusive);
        writer->append(damaged);
        writer.reset();
        Metrics metrics;
        BlockCache cache(0, metrics);
        EXPECT_THROW(
            {
                auto reader = std::make_shared<SSTableReader>(*fs, path, meta, cache, metrics);
                reader->verify();
            },
            Error);
    }
    std::filesystem::remove_all(dir);
}
TEST(SSTable, DisjointSequenceIteratorLoadsOnlyTheCurrentTable) {
    auto fs = makePosixFileSystem();
    auto dir = std::filesystem::temp_directory_path() / ("aegis-lazy-" + std::to_string(getpid()));
    std::filesystem::remove_all(dir);
    fs->createDirectories(dir);
    Metrics metrics;
    BlockCache cache(0, metrics);
    std::vector<std::shared_ptr<SSTableReader>> tables;
    for (std::uint64_t i = 1; i <= 3; ++i) {
        SSTableBuilder builder(*fs, dir, i, 32, 0.01);
        builder.add({std::string(1, static_cast<char>('a' + i - 1)), i, Kind::value,
                     std::string(100, 'v')});
        const auto meta = builder.finish(1);
        tables.push_back(
            std::make_shared<SSTableReader>(*fs, dir / fileName(i, ".sst"), meta, cache, metrics));
    }
    {
        TableSequenceIterator iterator(tables, "b");
        ASSERT_TRUE(iterator.valid());
        EXPECT_EQ(iterator.record().key, "b");
        EXPECT_EQ(metrics.cacheMisses.load(), 1);
        iterator.next();
        ASSERT_TRUE(iterator.valid());
        EXPECT_EQ(iterator.record().key, "c");
        EXPECT_EQ(metrics.cacheMisses.load(), 2);
        iterator.next();
        EXPECT_FALSE(iterator.valid());
        EXPECT_EQ(metrics.cacheMisses.load(), 2);
    }
    tables.clear();
    std::filesystem::remove_all(dir);
}
