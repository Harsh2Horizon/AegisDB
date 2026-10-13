#pragma once
#include "core/codec.h"
#include <aegisdb/filesystem.h>
#include <functional>

namespace aegisdb::detail {
constexpr std::size_t maxFrameBytes = 32 * 1024 * 1024;
class FramedLog {
  public:
    static std::unique_ptr<FramedLog> create(FileSystem &, const std::filesystem::path &,
                                             std::string_view magic);
    static std::uint64_t replay(File &, std::string_view magic, bool allowIncompleteTail,
                                const std::function<void(std::string_view)> &consume);
    explicit FramedLog(std::unique_ptr<File> file) : file_(std::move(file)) {}
    void append(std::string_view payload, bool sync);
    void sync() { file_->sync(); }
    std::uint64_t size() const { return file_->size(); }

  private:
    std::unique_ptr<File> file_;
};
std::string encodeBatch(const std::vector<Record> &);
std::vector<Record> decodeBatch(std::string_view);
constexpr std::string_view walMagic = "AEGWAL01";
constexpr std::string_view manifestMagic = "AEGMAN01";
} // namespace aegisdb::detail
