#include "fault_fs.h"
#include <aegisdb/db.h>
#include <fstream>
#include <gtest/gtest.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
using namespace aegisdb;
using namespace aegisdb::detail;
using namespace aegisdb::testing;
namespace {
std::filesystem::path temp(const char *name) {
    auto p = std::filesystem::temp_directory_path() /
             (std::string("aegis-recovery-") + std::to_string(getpid()) + name);
    std::filesystem::remove_all(p);
    return p;
}
Options opts() {
    Options o;
    o.automaticCompaction = false;
    o.levelCount = 3;
    return o;
}
void verify(const std::filesystem::path &path) {
    auto result = Database::open(path, opts());
    ASSERT_TRUE(result) << result.status().message;
    auto db = std::move(result).value();
    EXPECT_EQ(db->get("a").value(), "A");
    EXPECT_EQ(db->get("b").value(), "B");
    EXPECT_GE(db->statistics().lastSequence, 2);
    ASSERT_TRUE(db->put("c", "C"));
    EXPECT_GT(db->statistics().lastSequence, 2);
}
} // namespace
TEST(Recovery, SIGKILLAfterAcknowledgedBatch) {
    const auto path = temp("kill");
    int pipes[2];
    ASSERT_EQ(pipe(pipes), 0);
    const auto pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        close(pipes[0]);
        auto result = Database::open(path, opts());
        if (!result)
            std::_Exit(4);
        auto db = std::move(result).value();
        WriteBatch batch;
        batch.put("a", "A").put("b", "B");
        if (!db->write(batch))
            std::_Exit(5);
        const char ack = '!';
        if (::write(pipes[1], &ack, 1) != 1)
            std::_Exit(6);
        for (;;)
            pause();
    }
    close(pipes[1]);
    char ack = 0;
    ASSERT_EQ(::read(pipes[0], &ack, 1), 1);
    ASSERT_EQ(ack, '!');
    ASSERT_EQ(kill(pid, SIGKILL), 0);
    int status;
    ASSERT_EQ(waitpid(pid, &status, 0), pid);
    EXPECT_TRUE(WIFSIGNALED(status));
    close(pipes[0]);
    verify(path);
    std::filesystem::remove_all(path);
}
TEST(Recovery, CrashDuringFlushAndCompactionInstallation) {
    for (bool compact : {false, true})
        for (auto fault : {Fault::crashTableSync, Fault::crashManifestInstall}) {
            SCOPED_TRACE(std::string(compact ? "compact" : "flush") +
                         std::to_string(static_cast<int>(fault)));
            const auto path = temp("install");
            const auto pid = fork();
            ASSERT_GE(pid, 0);
            if (pid == 0) {
                auto fs = std::make_shared<FaultFileSystem>();
                auto o = opts();
                o.filesystem = fs;
                auto result = Database::open(path, o);
                if (!result)
                    std::_Exit(4);
                auto db = std::move(result).value();
                if (!db->put("a", "A") || !db->put("b", "B"))
                    std::_Exit(5);
                if (compact && !db->flush())
                    std::_Exit(6);
                fs->fault = fault;
                if (compact)
                    (void)db->compact();
                else
                    (void)db->flush();
                std::_Exit(7);
            }
            int status;
            ASSERT_EQ(waitpid(pid, &status, 0), pid);
            ASSERT_TRUE(WIFEXITED(status));
            ASSERT_EQ(WEXITSTATUS(status), 73);
            verify(path);
            std::filesystem::remove_all(path);
        }
}
TEST(Recovery, FailedSyncAndPartialBatchAreNotPublished) {
    for (auto fault : {Fault::walSync, Fault::walTornAppend}) {
        const auto path = temp("wal-fault");
        auto fs = std::make_shared<FaultFileSystem>();
        auto o = opts();
        o.filesystem = fs;
        auto result = Database::open(path, o);
        ASSERT_TRUE(result);
        auto db = std::move(result).value();
        ASSERT_TRUE(db->put("a", "A"));
        fs->fault = fault;
        WriteBatch batch;
        batch.put("b", "B").put("c", "C");
        EXPECT_EQ(db->write(batch).code, Code::io_error);
        EXPECT_EQ(db->get("b").status().code, Code::not_found);
        EXPECT_EQ(db->put("d", "D").code, Code::io_error);
        db.reset();
        result = Database::open(path, opts());
        ASSERT_TRUE(result) << result.status().message;
        db = std::move(result).value();
        EXPECT_EQ(db->get("a").value(), "A");
        if (fault == Fault::walTornAppend) {
            EXPECT_EQ(db->get("b").status().code, Code::not_found);
            EXPECT_EQ(db->get("c").status().code, Code::not_found);
        } else {
            EXPECT_EQ(db->get("b").value(), "B");
            EXPECT_EQ(db->get("c").value(), "C");
        }
        db.reset();
        std::filesystem::remove_all(path);
    }
}
TEST(Recovery, FailedFlushRetainsWALAndManifestAuthority) {
    for (auto fault : {Fault::tableAppend, Fault::manifestInstall}) {
        const auto path = temp("flush-fault");
        auto fs = std::make_shared<FaultFileSystem>();
        auto o = opts();
        o.filesystem = fs;
        auto result = Database::open(path, o);
        ASSERT_TRUE(result);
        auto db = std::move(result).value();
        ASSERT_TRUE(db->put("a", "A"));
        ASSERT_TRUE(db->put("b", "B"));
        fs->fault = fault;
        EXPECT_EQ(db->flush().code, Code::io_error);
        EXPECT_EQ(db->get("a").value(), "A");
        db.reset();
        verify(path);
        std::filesystem::remove_all(path);
    }
}
TEST(Recovery, MissingRequiredFilesAndCorruptFramesRejected) {
    for (auto suffix : {".sst", ".wal", "CURRENT"}) {
        const auto path = temp("missing");
        auto result = Database::open(path, opts());
        ASSERT_TRUE(result);
        auto db = std::move(result).value();
        ASSERT_TRUE(db->put("a", "A"));
        if (std::string(suffix) == ".sst")
            ASSERT_TRUE(db->flush());
        db.reset();
        bool removed = false;
        for (const auto &entry : std::filesystem::directory_iterator(path))
            if (entry.path().filename().string().ends_with(suffix)) {
                std::filesystem::remove(entry.path());
                removed = true;
                break;
            }
        ASSERT_TRUE(removed);
        auto reopened = Database::open(path, opts());
        EXPECT_EQ(reopened.status().code, Code::corruption);
        std::filesystem::remove_all(path);
    }
    const auto path = temp("corrupt");
    auto result = Database::open(path, opts());
    ASSERT_TRUE(result);
    auto db = std::move(result).value();
    ASSERT_TRUE(db->put("a", "A"));
    ASSERT_TRUE(db->put("b", "B"));
    db.reset();
    for (const auto &e : std::filesystem::directory_iterator(path))
        if (e.path().extension() == ".wal") {
            std::fstream file(e.path(), std::ios::binary | std::ios::in | std::ios::out);
            file.seekp(24);
            file.put('x');
        }
    EXPECT_EQ(Database::open(path, opts()).status().code, Code::corruption);
    std::filesystem::remove_all(path);
}
TEST(Recovery, TornManifestTailAndOrphanCleanup) {
    const auto path = temp("manifest-tail");
    auto result = Database::open(path, opts());
    ASSERT_TRUE(result);
    auto db = std::move(result).value();
    ASSERT_TRUE(db->put("a", "A"));
    ASSERT_TRUE(db->put("b", "B"));
    ASSERT_TRUE(db->flush());
    db.reset();
    auto fs = makePosixFileSystem();
    auto m = fs->open(path / "MANIFEST-000001", OpenMode::append_existing);
    m->append("xyz");
    m.reset();
    auto orphan = fs->open(path / fileName(999, ".sst.tmp"), OpenMode::create_exclusive);
    orphan->append("unfinished");
    orphan.reset();
    auto unrelated = fs->open(path / "notes.tmp", OpenMode::create_exclusive);
    unrelated->append("preserve unrelated names");
    unrelated.reset();
    verify(path);
    EXPECT_FALSE(fs->exists(path / fileName(999, ".sst.tmp")));
    EXPECT_TRUE(fs->exists(path / "notes.tmp"));
    Manifest manifest(*fs, path, 3);
    EXPECT_GT(manifest.state().nextFileNumber, 999);
    std::filesystem::remove_all(path);
}
TEST(Recovery, IncompleteUncommittedInitializationCanRestart) {
    Encoder header;
    header.raw(manifestMagic);
    header.u32(1);
    const auto bytes = checkedPayload(header.bytes());
    for (std::size_t length = 0; length <= bytes.size(); ++length) {
        const auto path = temp("initialization");
        auto fs = makePosixFileSystem();
        fs->createDirectories(path);
        auto file = fs->open(path / "MANIFEST-000001", OpenMode::create_exclusive);
        file->append(std::string_view(bytes).substr(0, length));
        file.reset();
        auto opened = Database::open(path, opts());
        ASSERT_TRUE(opened) << length << ":" << opened.status().message;
        auto db = std::move(opened).value();
        ASSERT_TRUE(db->put("a", "A"));
        ASSERT_TRUE(db->put("b", "B"));
        db.reset();
        verify(path);
        std::filesystem::remove_all(path);
    }
}
TEST(Recovery, ManifestRollbackCannotDiscardUnregisteredWalData) {
    const auto path = temp("rollback");
    auto result = Database::open(path, opts());
    ASSERT_TRUE(result);
    auto db = std::move(result).value();
    ASSERT_TRUE(db->put("a", "A"));
    db.reset();
    auto fs = makePosixFileSystem();
    auto manifest = fs->open(path / "MANIFEST-000001", OpenMode::append_existing);
    manifest->truncate(16);
    manifest->sync();
    manifest.reset();
    EXPECT_EQ(Database::open(path, opts()).status().code, Code::corruption);
    bool retained = false;
    for (const auto &entry : fs->list(path))
        if (numberedFileName(entry.filename().string(), ".wal")) {
            auto file = fs->open(entry, OpenMode::read_only);
            retained |= file->size() > 16;
        }
    EXPECT_TRUE(retained);
    std::filesystem::remove_all(path);
}
