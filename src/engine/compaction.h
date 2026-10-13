#pragma once
#include "storage/manifest.h"
#include <aegisdb/options.h>

namespace aegisdb::detail {
struct CompactionPlan {
    std::shared_ptr<const Version> version;
    std::vector<std::shared_ptr<SSTableReader>> inputs;
    std::size_t outputLevel{};
    Sequence oldestSnapshot{};
    bool baseLevelFor(std::string_view key) const;
};
class CompactionPicker {
  public:
    explicit CompactionPicker(const Options &options) : options_(options) {}
    std::optional<CompactionPlan> pick(std::shared_ptr<const Version>, Sequence oldest,
                                       std::optional<std::size_t> forcedLevel = {}) const;

  private:
    const Options &options_;
};
class CompactionExecutor {
  public:
    CompactionExecutor(FileSystem &fs, std::filesystem::path path, const Options &options,
                       Metrics &metrics)
        : fs_(fs), path_(std::move(path)), options_(options), metrics_(metrics) {}
    std::vector<TableMeta> run(const CompactionPlan &,
                               const std::function<std::uint64_t()> &allocate);

  private:
    FileSystem &fs_;
    std::filesystem::path path_;
    const Options &options_;
    Metrics &metrics_;
};
} // namespace aegisdb::detail
