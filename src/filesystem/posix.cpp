#include <aegisdb/filesystem.h>
#include <aegisdb/status.h>
#include <cerrno>
#include <fcntl.h>
#include <limits>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace aegisdb {

namespace {

[[noreturn]] void ioError(const char *action) {
    throw detail::Error(Code::io_error,
                        std::string(action) + ": " +
                            std::error_code(errno, std::generic_category()).message());
}

class Descriptor {
  public:
    explicit Descriptor(int fd) : fd_(fd) {
        if (fd < 0)
            ioError("open");
    }
    ~Descriptor() {
        if (fd_ >= 0)
            ::close(fd_);
    } // Never retry close: another thread could reuse the descriptor.
    Descriptor(const Descriptor &) = delete;
    Descriptor &operator=(const Descriptor &) = delete;
    int get() const { return fd_; }

  private:
    int fd_;
};

off_t checkedOffset(std::uint64_t value) {
    detail::require(value <= static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()),
                    Code::corruption, "file offset overflow");
    return static_cast<off_t>(value);
}

void syncFD(int fd) {
    int result;
    do {
        result = ::fsync(fd);
    } while (result < 0 && errno == EINTR);
    if (result < 0)
        ioError("fsync");
#if defined(__APPLE__)
    // macOS fsync alone does not require the drive to drain its volatile write cache.
    do {
        result = ::fcntl(fd, F_FULLFSYNC);
    } while (result < 0 && errno == EINTR);
    if (result < 0 && errno != EINVAL && errno != ENOTSUP)
        ioError("F_FULLFSYNC");
#endif
}

class PosixFile final : public File {
  public:
    PosixFile(const std::filesystem::path &p, OpenMode mode)
        : fd_(::open(p.c_str(), flags(mode), 0600)) {}
    std::string read(std::uint64_t offset, std::size_t length) const override {
        detail::require(length <= 64 * 1024 * 1024, Code::resource_exhausted,
                        "read exceeds maximum block");
        std::string bytes(length, '\0');
        std::size_t done = 0;
        while (done < length) {
            auto n = ::pread(fd_.get(), bytes.data() + done, length - done,
                             checkedOffset(offset + done));
            if (n < 0 && errno == EINTR)
                continue;
            if (n < 0)
                ioError("pread");
            if (n == 0)
                break;
            done += static_cast<std::size_t>(n);
        }
        bytes.resize(done);
        return bytes;
    }
    void append(std::string_view bytes) override {
        std::size_t done = 0;
        while (done < bytes.size()) {
            auto n = ::write(fd_.get(), bytes.data() + done, bytes.size() - done);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0)
                ioError("write");
            done += static_cast<std::size_t>(n);
        }
    }
    void sync() override { syncFD(fd_.get()); }
    void truncate(std::uint64_t length) override {
        int n;
        do {
            n = ::ftruncate(fd_.get(), checkedOffset(length));
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            ioError("truncate");
    }
    std::uint64_t size() const override {
        struct stat s{};
        if (::fstat(fd_.get(), &s) < 0)
            ioError("fstat");
        detail::require(s.st_size >= 0, Code::io_error, "negative file size");
        return static_cast<std::uint64_t>(s.st_size);
    }

  private:
    static int flags(OpenMode m) {
        return O_CLOEXEC |
               (m == OpenMode::read_only          ? O_RDONLY
                : m == OpenMode::create_exclusive ? O_WRONLY | O_CREAT | O_EXCL | O_APPEND
                                                  : O_RDWR | O_APPEND);
    }
    Descriptor fd_;
};


class PosixLock final : public DirectoryLock {
  public:
    explicit PosixLock(const std::filesystem::path &path)
        : fd_(::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600)) {
        if (::flock(fd_.get(), LOCK_EX | LOCK_NB) < 0) {
            if (errno == EWOULDBLOCK)
                throw detail::Error(Code::busy, "database already open");
            ioError("flock");
        }
    }

  private:
    Descriptor fd_;
};

class PosixFileSystem final : public FileSystem {
  public:
    std::unique_ptr<File> open(const std::filesystem::path &p, OpenMode m) override {
        return std::make_unique<PosixFile>(p, m);
    }
    void createDirectories(const std::filesystem::path &p) override {
        std::vector<std::filesystem::path> created;
        auto parent = std::filesystem::absolute(p);
        while (!std::filesystem::exists(parent)) {
            created.push_back(parent);
            parent = parent.parent_path();
        }
        std::filesystem::create_directories(p);
        // Persist each new path component, including the database directory's name in its parent.
        for (auto it = created.rbegin(); it != created.rend(); ++it)
            syncDirectory(it->parent_path());
    }
    bool exists(const std::filesystem::path &p) const override {
        return std::filesystem::exists(p);
    }
    std::vector<std::filesystem::path> list(const std::filesystem::path &p) const override {
        std::vector<std::filesystem::path> r;
        for (const auto &e : std::filesystem::directory_iterator(p))
            r.push_back(e.path());
        return r;
    }
    void rename(const std::filesystem::path &a, const std::filesystem::path &b) override {
        std::filesystem::rename(a, b);
    }
    void remove(const std::filesystem::path &p) override { std::filesystem::remove(p); }
    void syncDirectory(const std::filesystem::path &p) override {
        Descriptor fd(::open(p.c_str(), O_RDONLY | O_CLOEXEC));
        int n;
        do {
            n = ::fsync(fd.get());
        } while (n < 0 && errno == EINTR);
        if (n < 0)
            ioError("directory fsync");
    }
    std::unique_ptr<DirectoryLock> lock(const std::filesystem::path &p) override {
        return std::make_unique<PosixLock>(p / "LOCK");
    }
};
} // namespace
std::shared_ptr<FileSystem> makePosixFileSystem() { return std::make_shared<PosixFileSystem>(); }
} // namespace aegisdb
