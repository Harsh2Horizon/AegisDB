#include "storage/log.h"

namespace aegisdb::detail {
std::unique_ptr<FramedLog> FramedLog::create(FileSystem &fs, const std::filesystem::path &path,
                                             std::string_view magic) {
    require(magic.size() == 8, Code::internal_error, "invalid log magic");
    auto file = fs.open(path, OpenMode::create_exclusive);
    Encoder e;
    e.raw(magic);
    e.u32(1);
    file->append(checkedPayload(e.bytes()));
    file->sync();
    fs.syncDirectory(path.parent_path());
    return std::make_unique<FramedLog>(std::move(file));
}
std::uint64_t FramedLog::replay(File &file, std::string_view magic, bool allowIncompleteTail,
                                const std::function<void(std::string_view)> &consume) {
    const auto size = file.size();
    const auto header = file.read(0, 16);
    require(header.size() == 16, Code::corruption, "incomplete log header");
    Decoder h(verifyPayload(header));
    require(h.raw(8) == magic && h.u32() == 1, Code::corruption, "unsupported log format");
    h.finish();
    std::uint64_t offset = 16;
    while (offset < size) {
        const auto frame = file.read(offset, 12);
        if (frame.size() < 12) {
            require(allowIncompleteTail, Code::corruption, "incomplete non-tail log frame");
            break;
        }
        // Protect the length itself so corruption cannot masquerade as an incomplete tail.
        Decoder d(verifyPayload(frame));
        const auto length = d.u32();
        const auto checksum = d.u32();
        require(length > 0 && length <= maxFrameBytes, Code::corruption,
                "invalid log frame length");
        if (length > size - offset - 12) {
            require(allowIncompleteTail, Code::corruption, "incomplete non-tail log payload");
            break;
        }
        const auto payload = file.read(offset + 12, length);
        require(payload.size() == length, Code::corruption, "short log read");
        require(crc32c(payload) == checksum, Code::corruption, "log frame checksum mismatch");
        consume(payload);
        offset += 12 + length;
    }
    return offset;
}
void FramedLog::append(std::string_view payload, bool sync) {
    require(!payload.empty(), Code::invalid_argument, "empty log frame");
    require(payload.size() <= maxFrameBytes, Code::resource_exhausted,
            "log frame exceeds byte limit");
    Encoder e;
    e.u32(static_cast<std::uint32_t>(payload.size()));
    e.u32(crc32c(payload));
    e.u32(crc32c(e.bytes()));
    e.raw(payload);
    file_->append(e.bytes());
    if (sync)
        file_->sync();
}
std::string encodeBatch(const std::vector<Record> &records) {
    require(!records.empty() && records.size() <= UINT32_MAX, Code::invalid_argument,
            "invalid batch count");
    Encoder e;
    e.u8(1);
    e.u64(records.front().sequence);
    e.u32(static_cast<std::uint32_t>(records.size()));
    for (const auto &r : records)
        encodeRecord(e, r);
    return e.take();
}
std::vector<Record> decodeBatch(std::string_view payload) {
    Decoder d(payload);
    require(d.u8() == 1, Code::corruption, "invalid WAL frame type");
    const auto first = d.u64();
    const auto count = d.u32();
    require(count > 0 && count <= d.remaining() / 17 && first > 0 &&
                count - 1 <= maxSequence - first,
            Code::corruption, "invalid WAL sequence interval");
    std::vector<Record> records;
    records.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i) {
        auto r = decodeRecord(d);
        require(r.sequence == first + i, Code::corruption, "noncontiguous batch sequence");
        records.push_back(std::move(r));
    }
    d.finish();
    return records;
}
} // namespace aegisdb::detail
