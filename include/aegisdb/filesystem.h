#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace aegisdb {
// Infrastructure operations throw detail::Error. Public DB methods translate them into Status.
class File {
  public:
    virtual ~File() = default;
    virtual std::string read(std::uint64_t offset, std::size_t length) const = 0;
    virtual void append(std::string_view bytes) = 0;
    virtual void sync() = 0;
    virtual void truncate(std::uint64_t length) = 0;
    virtual std::uint64_t size() const = 0;
};

class DirectoryLock {
  public:
    virtual ~DirectoryLock() = default;
};

enum class OpenMode { read_only, create_exclusive, append_existing };
class FileSystem {
  public:
    virtual ~FileSystem() = default;
    virtual std::unique_ptr<File> open(const std::filesystem::path &, OpenMode) = 0;
    virtual void createDirectories(const std::filesystem::path &) = 0;
    virtual bool exists(const std::filesystem::path &) const = 0;
    virtual std::vector<std::filesystem::path> list(const std::filesystem::path &) const = 0;
    virtual void rename(const std::filesystem::path &from, const std::filesystem::path &to) = 0;
    virtual void remove(const std::filesystem::path &) = 0;
    virtual void syncDirectory(const std::filesystem::path &) = 0;
    virtual std::unique_ptr<DirectoryLock> lock(const std::filesystem::path &) = 0;
};
std::shared_ptr<FileSystem> makePosixFileSystem();
} // namespace aegisdb
