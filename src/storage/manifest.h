#pragma once
#include "storage/log.h"
#include "storage/sstable.h"
#include <optional>
#include <set>

namespace aegisdb::detail {
struct VersionEdit {
    std::vector<TableMeta> additions;
    std::vector<std::uint64_t> deletions, addLogs, deleteLogs;
    std::optional<std::uint64_t> logNumber, lastSequence, nextFileNumber;
};
struct ManifestState {
    std::vector<std::vector<TableMeta>> levels;
    std::set<std::uint64_t> logs;
    std::uint64_t logNumber{}, lastSequence{}, nextFileNumber{1};
};
class Manifest {
  public:
    Manifest(FileSystem &, std::filesystem::path directory, std::size_t levels);
    const ManifestState &state() const { return state_; }
    void commit(const VersionEdit &);
    std::uint64_t allocate(); // Caller serializes manifest operations.
  private:
    static void apply(ManifestState &, const VersionEdit &);
    FileSystem &fs_;
    std::filesystem::path directory_;
    ManifestState state_;
    std::unique_ptr<FramedLog> log_;
};
std::string encodeEdit(const VersionEdit &);
VersionEdit decodeEdit(std::string_view);
struct Version {
    std::vector<std::vector<std::shared_ptr<SSTableReader>>> levels;
};
std::shared_ptr<Version> applyVersion(const Version &, const VersionEdit &,
                                      const std::vector<std::shared_ptr<SSTableReader>> &additions);
} // namespace aegisdb::detail
