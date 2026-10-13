#include "core/codec.h"
#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <new>

namespace aegisdb::detail {

Status exceptionStatus() noexcept {
    try {
        try {
            throw;
        } catch (const Error &e) {
            return e.status;
        } catch (const std::filesystem::filesystem_error &e) {
            return {Code::io_error, e.code().message()};
        } catch (const std::bad_alloc &) {
            return {Code::resource_exhausted, "allocation failed"};
        } catch (const std::exception &) {
            return {Code::internal_error, "unexpected internal error"};
        } catch (...) {
            return {Code::internal_error, "unknown internal error"};
        }
    } catch (...) {
        // Even an error message allocation can fail. An empty message keeps this boundary noexcept.
        return {Code::resource_exhausted, {}};
    }
}

// 
void Encoder::u32(std::uint32_t v) {
    for (unsigned i = 0; i < 4; ++i)
        u8(static_cast<std::uint8_t>(v >> (i * 8)));
}

void Encoder::u64(std::uint64_t v) {
    for (unsigned i = 0; i < 8; ++i)
        u8(static_cast<std::uint8_t>(v >> (i * 8)));
}
void Encoder::string(std::string_view v) {
    require(v.size() <= UINT32_MAX, Code::invalid_argument, "string exceeds encoding limit");
    u32(static_cast<std::uint32_t>(v.size()));
    raw(v);
}
std::string_view Decoder::raw(std::size_t n) {
    require(n <= remaining(), Code::corruption, "truncated encoding");
    auto v = bytes_.substr(position_, n);
    position_ += n;
    return v;
}
std::uint8_t Decoder::u8() { return static_cast<std::uint8_t>(raw(1)[0]); }
std::uint32_t Decoder::u32() {
    std::uint32_t v = 0;
    for (unsigned i = 0; i < 4; ++i)
        v |= std::uint32_t(u8()) << (i * 8);
    return v;
}
std::uint64_t Decoder::u64() {
    std::uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i)
        v |= std::uint64_t(u8()) << (i * 8);
    return v;
}
std::string Decoder::string(std::size_t maximum) {
    const auto n = u32();
    require(n <= maximum, Code::corruption, "oversized encoded string");
    return std::string(raw(n));
}

constexpr auto crcTable() {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        auto c = i;
        for (int j = 0; j < 8; ++j)
            c = (c >> 1) ^ ((c & 1) ? 0x82f63b78U : 0U);
        table[i] = c;
    }
    return table;
}

std::uint32_t crc32c(std::string_view bytes) noexcept {
    constexpr auto table = crcTable();
    std::uint32_t c = ~0U;
    for (char byte : bytes)
        c = table[(c ^ static_cast<unsigned char>(byte)) & 255] ^ (c >> 8);
    return ~c;
}

void encodeRecord(Encoder &e, const Record &r) {
    require(r.key.size() <= encodedKeyLimit && r.value.size() <= encodedValueLimit &&
                r.sequence > 0 && (r.kind == Kind::value || r.kind == Kind::deletion) &&
                (r.kind != Kind::deletion || r.value.empty()),
            Code::invalid_argument, "record exceeds format bounds");
    e.string(r.key);
    e.u64(r.sequence);
    e.u8(static_cast<std::uint8_t>(r.kind));
    e.string(r.value);
}
Record decodeRecord(Decoder &d) {
    Record r;
    r.key = d.string(encodedKeyLimit);
    r.sequence = d.u64();
    r.kind = static_cast<Kind>(d.u8());
    r.value = d.string(encodedValueLimit);
    require(r.sequence > 0 && (r.kind == Kind::value || r.kind == Kind::deletion), Code::corruption,
            "invalid record");
    require(r.kind != Kind::deletion || r.value.empty(), Code::corruption, "nonempty tombstone");
    return r;
}
std::string checkedPayload(std::string_view payload) {
    Encoder e;
    e.raw(payload);
    e.u32(crc32c(payload));
    return e.take();
}
std::string_view verifyPayload(std::string_view bytes) {
    require(bytes.size() >= 4, Code::corruption, "missing checksum");
    const auto payload = bytes.substr(0, bytes.size() - 4);
    Decoder d(bytes.substr(bytes.size() - 4));
    require(d.u32() == crc32c(payload), Code::corruption, "checksum mismatch");
    return payload;
}
std::string fileName(std::uint64_t id, std::string_view extension) {
    char out[32];
    std::snprintf(out, sizeof(out), "%020llu", static_cast<unsigned long long>(id));
    return std::string(out) + std::string(extension);
}
bool numberedFileName(std::string_view name, std::string_view extension) noexcept {
    return name.size() == 20 + extension.size() && name.ends_with(extension) &&
           std::all_of(name.begin(), name.begin() + 20,
                       [](char c) { return c >= '0' && c <= '9'; });
}
} // namespace aegisdb::detail
