#include "fault_fs.h"
#include "storage/manifest.h"
#include <aegisdb/db.h>
#include <atomic>
#include <barrier>
#include <fstream>
#include <gtest/gtest.h>
#include <random>
#include <thread>
#include <unistd.h>

using namespace aegisdb;
using namespace aegisdb::detail;
class EngineTest : public ::testing::Test {
  protected:
    std::filesystem::path path;
    Options options;
    std::unique_ptr<Database> db;
    void SetUp() override {
        path = std::filesystem::temp_directory_path() /
               ("aegis-engine-" + std::to_string(getpid()) + "-" +
                ::testing::UnitTest::GetInstance()->current_test_info()->name());
        std::filesystem::remove_all(path);
        options.memtableBytes = 8192;
        options.tableBytes = 4096;
        options.blockBytes = 256;
        options.levelOneBytes = 8192;
        options.levelCount = 4;
        open();
    }
    void TearDown() override {
        if (auto fs =
                std::dynamic_pointer_cast<aegisdb::testing::FaultFileSystem>(options.filesystem)) {
            {
                std::lock_guard lock(fs->gateMutex);
                fs->blockTables = false;
            }
            fs->gateCV.notify_all();
        }
        db.reset();
        std::filesystem::remove_all(path);
    }
    void open() {
        auto result = Database::open(path, options);
        ASSERT_TRUE(result.ok()) << result.status().message;
        db = std::move(result).value();
    }
    void reopen() {
        db.reset();
        open();
    }
};
TEST_F(EngineTest, PersistenceUpdatesDeletionSnapshotsAndCompaction) {
    ASSERT_TRUE(db->put("x", "A"));
    auto s1 = db->createSnapshot();
    ASSERT_TRUE(s1);
    ASSERT_TRUE(db->flush());
    ASSERT_TRUE(db->compact());
    ASSERT_TRUE(db->put("x", "B"));
    auto s2 = db->createSnapshot();
    ASSERT_TRUE(s2);
    ASSERT_TRUE(db->flush());
    ASSERT_TRUE(db->put("x", "C"));
    ASSERT_TRUE(db->remove("x"));
    ASSERT_TRUE(db->flush());
    ASSERT_TRUE(db->compact());
    EXPECT_EQ(db->get("x").status().code, Code::not_found);
    EXPECT_EQ(db->get("x", {s1.value()}).value(), "A");
    EXPECT_EQ(db->get("x", {s2.value()}).value(), "B");
    s1 = Status{Code::not_found, "released"};
    s2 = Status{Code::not_found, "released"};
    ASSERT_TRUE(db->compact());
    reopen();
    EXPECT_EQ(db->get("x").status().code, Code::not_found);
    ASSERT_TRUE(db->put("x", "new"));
    reopen();
    EXPECT_EQ(db->get("x").value(), "new");
}
TEST_F(EngineTest, BinaryKeysBatchAndBoundedPagination) {
    WriteBatch b;
    b.put("", "")
        .put(std::string("\0\xff", 2), std::string("\xff\0", 2))
        .put("a", "A")
        .put("b", "B")
        .remove("a")
        .put("z", "Z");
    ASSERT_TRUE(db->write(b));
    auto snap = db->createSnapshot();
    ASSERT_TRUE(snap);
    ASSERT_TRUE(db->put("b", "new"));
    ASSERT_TRUE(db->compact());
    ScanOptions scan;
    scan.snapshot = snap.value();
    scan.limit = 2;
    auto p1 = db->scan(scan);
    ASSERT_TRUE(p1);
    ASSERT_EQ(p1.value().entries.size(), 2);
    EXPECT_EQ(p1.value().entries[0].key, "");
    ASSERT_TRUE(p1.value().nextAfter);
    scan.after = p1.value().nextAfter;
    auto p2 = db->scan(scan);
    ASSERT_TRUE(p2);
    ASSERT_EQ(p2.value().entries.size(), 2);
    EXPECT_EQ(p2.value().entries[0].value, "B");
    EXPECT_EQ(p2.value().entries[1].key, "z");
    EXPECT_EQ(db->get(std::string("\0\xff", 2)).value(), std::string("\xff\0", 2));
    auto other = Database::open(path, options);
    EXPECT_EQ(other.status().code, Code::busy);
    reopen();
    EXPECT_EQ(db->get("b", {snap.value()}).status().code, Code::invalid_argument);
}
TEST_F(EngineTest, SnapshotReleaseReclaimsBottomTombstonesAndEmptyOutputs) {
    ASSERT_TRUE(db->put("only", "old"));
    auto result = db->createSnapshot();
    ASSERT_TRUE(result);
    Snapshot snapshot = result.value();
    ASSERT_TRUE(db->compact());
    ASSERT_TRUE(db->remove("only"));
    ASSERT_TRUE(db->compact());
    EXPECT_EQ(db->get("only", {snapshot}).value(), "old");
    snapshot = {};
    result = Status{Code::not_found, "released"};
    ASSERT_TRUE(db->compact());
    for (const auto &level : db->statistics().levels)
        EXPECT_EQ(level.tables, 0);
    reopen();
    EXPECT_EQ(db->get("only").status().code, Code::not_found);
}
TEST_F(EngineTest, ManualCompactionRelievesTheL0Bound) {
    db.reset();
    std::filesystem::remove_all(path);
    options.automaticCompaction = false;
    options.l0CompactionFiles = 1;
    options.l0StopFiles = 2;
    options.writeTimeout = std::chrono::milliseconds(30);
    open();
    ASSERT_TRUE(db->put("a", "A"));
    ASSERT_TRUE(db->flush());
    ASSERT_TRUE(db->put("b", "B"));
    ASSERT_TRUE(db->flush());
    EXPECT_EQ(db->put("c", "C").code, Code::busy);
    ASSERT_TRUE(db->health());
    ASSERT_TRUE(db->compact());
    ASSERT_TRUE(db->put("c", "C"));
}
TEST_F(EngineTest, DiscoveredReadCorruptionDisablesFurtherWrites) {
    db.reset();
    std::filesystem::remove_all(path);
    options.automaticCompaction = false;
    options.blockCacheBytes = 0;
    open();
    ASSERT_TRUE(db->put("a", "A"));
    ASSERT_TRUE(db->flush());
    for (const auto &entry : std::filesystem::directory_iterator(path))
        if (entry.path().extension() == ".sst") {
            std::fstream file(entry.path(), std::ios::binary | std::ios::in | std::ios::out);
            file.seekg(16);
            char byte{};
            file.get(byte);
            file.seekp(16);
            file.put(static_cast<char>(byte ^ 1));
        }
    EXPECT_EQ(db->get("a").status().code, Code::corruption);
    EXPECT_EQ(db->health().code, Code::corruption);
    EXPECT_EQ(db->put("b", "B").code, Code::corruption);
    db.reset();
    EXPECT_EQ(Database::open(path, options).status().code, Code::corruption);
}
TEST_F(EngineTest, AutomaticRotationAndLevelInvariants) {
    for (int round = 0; round < 6; ++round)
        for (int i = 0; i < 150; ++i)
            ASSERT_TRUE(db->put(std::to_string(i), std::string(70, static_cast<char>('a' + round)),
                                {false}));
    ASSERT_TRUE(db->flush());
    ASSERT_TRUE(db->compact());
    reopen();
    for (int i = 0; i < 150; ++i)
        EXPECT_EQ(db->get(std::to_string(i)).value(), std::string(70, 'f'));
    const auto tableCount = db->statistics().levels.back().tables;
    db.reset();
    auto fs = makePosixFileSystem();
    Manifest manifest(*fs, path, options.levelCount);
    for (std::size_t l = 1; l < manifest.state().levels.size(); ++l) {
        const auto &files = manifest.state().levels[l];
        for (std::size_t i = 1; i < files.size(); ++i)
            EXPECT_LT(files[i - 1].largest, files[i].smallest);
    }
    EXPECT_GT(tableCount, 0);
}
TEST_F(EngineTest, AtomicVisibilityConcurrentWritesAndScans) {
    WriteBatch initial;
    initial.put("a", "0").put("b", "0");
    ASSERT_TRUE(db->write(initial));
    std::atomic<bool> finished = false;
    std::atomic<int> mismatch = 0;
    std::barrier start(3);
    std::jthread reader([&] {
        start.arrive_and_wait();
        while (!finished) {
            ScanOptions s;
            s.start = "a";
            s.end = "c";
            auto p = db->scan(s);
            if (!p || p.value().entries.size() != 2 ||
                p.value().entries[0].value != p.value().entries[1].value)
                ++mismatch;
        }
    });
    std::jthread writer([&] {
        start.arrive_and_wait();
        for (int i = 1; i < 300; ++i) {
            WriteBatch b;
            b.put("a", std::to_string(i)).put("b", std::to_string(i));
            if (!db->write(b, {false}))
                ++mismatch;
        }
        finished = true;
    });
    start.arrive_and_wait();
    writer.join();
    reader.join();
    EXPECT_EQ(mismatch, 0);
    ASSERT_TRUE(db->compact());
    EXPECT_EQ(db->get("a").value(), "299");
}
TEST_F(EngineTest, ManyWritersReadersAndSnapshotsDuringMaintenance) {
    std::atomic<int> failures = 0, remaining = 4;
    std::barrier begin(6);
    std::vector<std::jthread> writers;
    for (int t = 0; t < 4; ++t)
        writers.emplace_back([&, t] {
            begin.arrive_and_wait();
            for (int i = 0; i < 120; ++i) {
                WriteBatch b;
                const auto prefix = "t" + std::to_string(t);
                b.put(prefix + ":a", std::to_string(i)).put(prefix + ":b", std::to_string(i));
                if (!db->write(b, {false}))
                    ++failures;
            }
            --remaining;
        });
    std::jthread reader([&] {
        begin.arrive_and_wait();
        while (remaining) {
            auto snap = db->createSnapshot();
            if (!snap) {
                ++failures;
                continue;
            }
            ScanOptions s;
            s.start = "t";
            s.end = "u";
            s.snapshot = snap.value();
            auto page = db->scan(s);
            if (!page || page.value().entries.size() % 2 != 0) {
                ++failures;
                continue;
            }
            for (std::size_t i = 0; i < page.value().entries.size(); i += 2)
                if (page.value().entries[i].value != page.value().entries[i + 1].value)
                    ++failures;
        }
    });
    begin.arrive_and_wait();
    for (int i = 0; i < 4; ++i)
        if (!db->compact())
            ++failures;
    for (auto &writer : writers)
        writer.join();
    reader.join();
    EXPECT_EQ(failures, 0);
    reopen();
    for (int t = 0; t < 4; ++t)
        EXPECT_EQ(db->get("t" + std::to_string(t) + ":a").value(), "119");
}
TEST_F(EngineTest, BoundedQueueTimeoutDoesNotCommitOrPoison) {
    db.reset();
    std::filesystem::remove_all(path);
    auto fs = std::make_shared<aegisdb::testing::FaultFileSystem>();
    options.filesystem = fs;
    options.automaticCompaction = false;
    options.memtableBytes = 1;
    options.maxImmutableMemtables = 2;
    options.writeTimeout = std::chrono::milliseconds(30);
    open();
    {
        std::lock_guard lock(fs->gateMutex);
        fs->blockTables = true;
    }
    ASSERT_TRUE(db->put("a", "A", {false}));
    {
        std::unique_lock lock(fs->gateMutex);
        ASSERT_TRUE(
            fs->gateCV.wait_for(lock, std::chrono::seconds(5), [&] { return fs->tableBlocked; }));
    }
    ASSERT_TRUE(db->put("b", "B", {false}));
    const auto committed = db->statistics().lastSequence;
    EXPECT_EQ(db->put("c", "C").code, Code::busy);
    EXPECT_EQ(db->statistics().lastSequence, committed);
    EXPECT_TRUE(db->health());
    {
        std::lock_guard lock(fs->gateMutex);
        fs->blockTables = false;
    }
    fs->gateCV.notify_all();
    ASSERT_TRUE(db->flush());
    ASSERT_TRUE(db->put("c", "C"));
}
TEST_F(EngineTest, DeterministicDifferentialWithHistory) {
    constexpr unsigned seed = 0xA615;
    std::mt19937 rng(seed);
    SCOPED_TRACE("seed=" + std::to_string(seed));
    std::map<std::string, std::string> model;
    std::vector<std::pair<Snapshot, std::map<std::string, std::string>>> snaps;
    auto compare = [&](const Snapshot &s, const auto &expected) {
        ScanOptions scan;
        scan.snapshot = s;
        scan.limit = 10000;
        auto page = db->scan(scan);
        ASSERT_TRUE(page) << page.status().message;
        std::vector<KeyValue> entries;
        for (const auto &[k, v] : expected)
            entries.push_back({k, v});
        EXPECT_EQ(page.value().entries, entries);
        for (int k = 0; k < 30; ++k) {
            const auto key = std::to_string(k);
            auto result = db->get(key, {s});
            auto it = expected.find(key);
            if (it == expected.end())
                EXPECT_EQ(result.status().code, Code::not_found);
            else {
                ASSERT_TRUE(result);
                EXPECT_EQ(result.value(), it->second);
            }
        }
    };
    std::string trace;
    for (int step = 0; step < 1200; ++step) {
        const auto op = rng() % 12;
        const auto key = std::to_string(rng() % 30);
        trace += std::to_string(step) + ":" + std::to_string(op) + ":" + key + " ";
        SCOPED_TRACE(trace);
        if (op < 4) {
            const auto value = std::string(rng() % 90, static_cast<char>(rng() % 256));
            ASSERT_TRUE(db->put(key, value, {false}));
            model[key] = value;
        } else if (op < 6) {
            ASSERT_TRUE(db->remove(key, {false}));
            model.erase(key);
        } else if (op == 6) {
            WriteBatch b;
            b.put(key, "batch").remove("29").put("29", "tail");
            ASSERT_TRUE(db->write(b, {false}));
            model[key] = "batch";
            model["29"] = "tail";
        } else if (op == 7 && snaps.size() < 6) {
            auto s = db->createSnapshot();
            ASSERT_TRUE(s);
            snaps.push_back({s.value(), model});
        } else if (op == 8) {
            ASSERT_TRUE(db->flush());
        } else if (op == 9) {
            ASSERT_TRUE(db->compact());
        } else if (op == 10) {
            snaps.clear();
            reopen();
        } else {
            compare({}, model);
            for (const auto &[s, m] : snaps)
                compare(s, m);
            if (!snaps.empty())
                snaps.erase(snaps.begin());
        }
    }
    compare({}, model);
    for (const auto &[s, m] : snaps)
        compare(s, m);
}
