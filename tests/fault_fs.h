#pragma once
#include "storage/manifest.h"
#include <aegisdb/filesystem.h>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <mutex>

namespace aegisdb::testing {
enum class Fault {
    none,
    walSync,
    walTornAppend,
    tableAppend,
    manifestInstall,
    crashTableSync,
    crashManifestInstall
};
class FaultFileSystem final : public FileSystem {
  public:
    std::atomic<Fault> fault{Fault::none};
    std::mutex gateMutex;
    std::condition_variable gateCV;
    bool blockTables{}, tableBlocked{};
    std::shared_ptr<FileSystem> base{makePosixFileSystem()};
    class WrappedFile final : public File {
      public:
        WrappedFile(std::unique_ptr<File> file, std::string name, FaultFileSystem &owner)
            : file_(std::move(file)), name_(std::move(name)), owner_(owner) {}
        std::string read(std::uint64_t offset, std::size_t size) const override {
            return file_->read(offset, size);
        }
        void append(std::string_view bytes) override {
            if (name_.ends_with(".sst.tmp")) {
                std::unique_lock lock(owner_.gateMutex);
                if (owner_.blockTables) {
                    owner_.tableBlocked = true;
                    owner_.gateCV.notify_all();
                    owner_.gateCV.wait(lock, [this] { return !owner_.blockTables; });
                }
            }
            auto fault = owner_.fault.load();
            if (name_.ends_with(".wal") && fault == Fault::walTornAppend && consume(fault)) {
                file_->append(bytes.substr(0, bytes.size() / 2));
                throw detail::Error(Code::io_error, "injected partial append");
            }
            if (name_.ends_with(".sst.tmp") && fault == Fault::tableAppend && consume(fault))
                throw detail::Error(Code::io_error, "injected table write failure");
            bool install = false;
            if (name_ == "MANIFEST-000001" &&
                (fault == Fault::manifestInstall || fault == Fault::crashManifestInstall)) {
                detail::Decoder d(bytes);
                (void)d.u32();
                (void)d.u32();
                (void)d.u32();
                const auto e = detail::decodeEdit(d.raw(d.remaining()));
                install = !e.additions.empty() || !e.deletions.empty() || !e.deleteLogs.empty();
            }
            if (install && fault == Fault::manifestInstall && consume(fault))
                throw detail::Error(Code::io_error, "injected manifest installation failure");
            file_->append(bytes);
            if (install && fault == Fault::crashManifestInstall && consume(fault)) {
                file_->sync();
                std::_Exit(73);
            }
        }
        void sync() override {
            auto fault = owner_.fault.load();
            if (name_.ends_with(".wal") && fault == Fault::walSync && consume(fault))
                throw detail::Error(Code::io_error, "injected WAL sync failure");
            file_->sync();
            if (name_.ends_with(".sst.tmp") && fault == Fault::crashTableSync && consume(fault))
                std::_Exit(73);
        }
        void truncate(std::uint64_t length) override { file_->truncate(length); }
        std::uint64_t size() const override { return file_->size(); }

      private:
        bool consume(Fault f) { return owner_.fault.compare_exchange_strong(f, Fault::none); }
        std::unique_ptr<File> file_;
        std::string name_;
        FaultFileSystem &owner_;
    };
    std::unique_ptr<File> open(const std::filesystem::path &p, OpenMode mode) override {
        return std::make_unique<WrappedFile>(base->open(p, mode), p.filename().string(), *this);
    }
    void createDirectories(const std::filesystem::path &p) override { base->createDirectories(p); }
    bool exists(const std::filesystem::path &p) const override { return base->exists(p); }
    std::vector<std::filesystem::path> list(const std::filesystem::path &p) const override {
        return base->list(p);
    }
    void rename(const std::filesystem::path &a, const std::filesystem::path &b) override {
        base->rename(a, b);
    }
    void remove(const std::filesystem::path &p) override { base->remove(p); }
    void syncDirectory(const std::filesystem::path &p) override { base->syncDirectory(p); }
    std::unique_ptr<DirectoryLock> lock(const std::filesystem::path &p) override {
        return base->lock(p);
    }
};
} // namespace aegisdb::testing
