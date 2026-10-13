#include "storage/manifest.h"
#include <algorithm>
#include <charconv>

namespace aegisdb::detail {
namespace {
void encodeNumbers(Encoder &e, const std::vector<std::uint64_t> &numbers) {
    require(numbers.size() <= UINT32_MAX, Code::resource_exhausted, "manifest edit too large");
    e.u32(static_cast<std::uint32_t>(numbers.size()));
    for (auto n : numbers)
        e.u64(n);
}
std::vector<std::uint64_t> decodeNumbers(Decoder &d) {
    const auto count = d.u32();
    require(count <= d.remaining() / 8, Code::corruption, "invalid manifest count");
    std::vector<std::uint64_t> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
        result.push_back(d.u64());
    return result;
}
} // namespace
std::string encodeEdit(const VersionEdit &edit) {
    Encoder e;
    e.u8(1);
    e.u8(static_cast<std::uint8_t>((edit.logNumber ? 1 : 0) | (edit.lastSequence ? 2 : 0) |
                                   (edit.nextFileNumber ? 4 : 0)));
    if (edit.logNumber)
        e.u64(*edit.logNumber);
    if (edit.lastSequence)
        e.u64(*edit.lastSequence);
    if (edit.nextFileNumber)
        e.u64(*edit.nextFileNumber);
    e.u32(static_cast<std::uint32_t>(edit.additions.size()));
    for (const auto &meta : edit.additions)
        encodeTableMeta(e, meta);
    encodeNumbers(e, edit.deletions);
    encodeNumbers(e, edit.addLogs);
    encodeNumbers(e, edit.deleteLogs);
    return e.take();
}
VersionEdit decodeEdit(std::string_view bytes) {
    Decoder d(bytes);
    require(d.u8() == 1, Code::corruption, "unsupported version edit");
    const auto mask = d.u8();
    require(mask <= 7, Code::corruption, "invalid edit flags");
    VersionEdit e;
    if (mask & 1)
        e.logNumber = d.u64();
    if (mask & 2)
        e.lastSequence = d.u64();
    if (mask & 4)
        e.nextFileNumber = d.u64();
    const auto count = d.u32();
    require(count <= d.remaining() / 44, Code::corruption, "invalid table count");
    e.additions.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
        e.additions.push_back(decodeTableMeta(d));
    e.deletions = decodeNumbers(d);
    e.addLogs = decodeNumbers(d);
    e.deleteLogs = decodeNumbers(d);
    d.finish();
    return e;
}
void Manifest::apply(ManifestState &state, const VersionEdit &edit) {
    if (edit.nextFileNumber) {
        require(*edit.nextFileNumber >= state.nextFileNumber, Code::corruption,
                "file counter regression");
        state.nextFileNumber = *edit.nextFileNumber;
    }
    if (edit.lastSequence) {
        require(*edit.lastSequence >= state.lastSequence, Code::corruption, "sequence regression");
        state.lastSequence = *edit.lastSequence;
    }
    for (auto number : edit.deletions) {
        bool found = false;
        for (auto &level : state.levels) {
            const auto it = std::find_if(level.begin(), level.end(),
                                         [number](const auto &m) { return m.number == number; });
            if (it != level.end()) {
                require(!found, Code::corruption, "duplicate file identity");
                level.erase(it);
                found = true;
            }
        }
        require(found, Code::corruption, "deleting absent table");
    }
    std::set<std::uint64_t> ids;
    for (const auto &level : state.levels)
        for (const auto &m : level)
            ids.insert(m.number);
    for (const auto &m : edit.additions) {
        require(m.level < state.levels.size() && m.number < state.nextFileNumber &&
                    m.minSequence > 0 && m.maxSequence <= state.lastSequence &&
                    ids.insert(m.number).second,
                Code::corruption, "invalid installed table");
        state.levels[m.level].push_back(m);
    }
    for (auto id : edit.addLogs)
        require(id > 0 && id < state.nextFileNumber && state.logs.insert(id).second,
                Code::corruption, "invalid WAL addition");
    for (auto id : edit.deleteLogs)
        require(state.logs.erase(id) == 1, Code::corruption, "deleting absent WAL");
    if (edit.logNumber)
        state.logNumber = *edit.logNumber;
    require(state.logs.empty() ? state.logNumber == 0 : state.logNumber == *state.logs.begin(),
            Code::corruption, "WAL floor mismatch");
    for (std::size_t l = 1; l < state.levels.size(); ++l) {
        auto &level = state.levels[l];
        std::sort(level.begin(), level.end(),
                  [](const auto &a, const auto &b) { return a.smallest < b.smallest; });
        for (std::size_t i = 1; i < level.size(); ++i)
            require(level[i - 1].largest < level[i].smallest, Code::corruption,
                    "overlapping leveled files");
    }
}
Manifest::Manifest(FileSystem &fs, std::filesystem::path directory, std::size_t levels)
    : fs_(fs), directory_(std::move(directory)) {
    state_.levels.resize(levels);
    const auto current = directory_ / "CURRENT", path = directory_ / "MANIFEST-000001";
    if (!fs_.exists(current)) {
        // A crash before CURRENT installation may leave only uncommitted initialization artifacts.
        for (const auto &entry : fs_.list(directory_)) {
            const auto name = entry.filename().string();
            require(!numberedFileName(name, ".wal") && !numberedFileName(name, ".sst") &&
                        !numberedFileName(name, ".sst.tmp"),
                    Code::corruption, "CURRENT missing from populated database");
        }
        if (fs_.exists(path)) {
            auto previous = fs_.open(path, OpenMode::read_only);
            require(previous->size() <= 16, Code::corruption,
                    "CURRENT missing from initialized database");
            previous.reset();
            fs_.remove(path);
        }
        if (fs_.exists(directory_ / "CURRENT.tmp"))
            fs_.remove(directory_ / "CURRENT.tmp");
        log_ = FramedLog::create(fs_, path, manifestMagic);
        auto pointer = fs_.open(directory_ / "CURRENT.tmp", OpenMode::create_exclusive);
        pointer->append("MANIFEST-000001\n");
        pointer->sync();
        pointer.reset();
        fs_.rename(directory_ / "CURRENT.tmp", current);
        fs_.syncDirectory(directory_);
    } else {
        auto pointer = fs_.open(current, OpenMode::read_only);
        require(pointer->size() == 16 && pointer->read(0, 16) == "MANIFEST-000001\n",
                Code::corruption, "invalid CURRENT pointer");
        require(fs_.exists(path), Code::corruption, "required manifest missing");
        auto file = fs_.open(path, OpenMode::append_existing);
        const auto valid =
            FramedLog::replay(*file, manifestMagic, true,
                              [this](std::string_view bytes) { apply(state_, decodeEdit(bytes)); });
        if (valid != file->size()) {
            file->truncate(valid);
            file->sync();
        }
        log_ = std::make_unique<FramedLog>(std::move(file));
    }
    // Orphans also reserve their identity. This avoids reuse even after a pre-installation crash.
    auto next = state_.nextFileNumber;
    for (const auto &p : fs_.list(directory_)) {
        const auto name = p.filename().string();
        if (name.size() < 21 || name[20] != '.')
            continue;
        std::uint64_t id = 0;
        const auto parsed = std::from_chars(name.data(), name.data() + 20, id);
        if (parsed.ec == std::errc{} && parsed.ptr == name.data() + 20) {
            require(id < UINT64_MAX, Code::corruption, "file identity overflow");
            next = std::max(next, id + 1);
        }
    }
    if (next > state_.nextFileNumber) {
        VersionEdit e;
        e.nextFileNumber = next;
        commit(e);
    }
}
void Manifest::commit(const VersionEdit &edit) {
    auto next = state_;
    apply(next, edit);
    const auto payload = encodeEdit(edit);
    log_->append(payload, true);
    state_ = std::move(next);
}
std::uint64_t Manifest::allocate() {
    const auto n = state_.nextFileNumber;
    require(n < UINT64_MAX, Code::resource_exhausted, "file identity exhausted");
    VersionEdit e;
    e.nextFileNumber = n + 1;
    commit(e);
    return n;
}
std::shared_ptr<Version>
applyVersion(const Version &current, const VersionEdit &edit,
             const std::vector<std::shared_ptr<SSTableReader>> &additions) {
    auto next = std::make_shared<Version>(current);
    for (auto &level : next->levels)
        std::erase_if(level, [&edit](const auto &table) {
            return std::find(edit.deletions.begin(), edit.deletions.end(),
                             table->metadata().number) != edit.deletions.end();
        });
    for (const auto &table : additions)
        next->levels.at(table->metadata().level).push_back(table);
    for (std::size_t l = 1; l < next->levels.size(); ++l)
        std::sort(next->levels[l].begin(), next->levels[l].end(), [](const auto &a, const auto &b) {
            return a->metadata().smallest < b->metadata().smallest;
        });
    return next;
}
} // namespace aegisdb::detail
