#pragma once
#include "core/record.h"
#include <span>
#include <vector>

namespace aegisdb::detail {

class Encoder {
  public:
    void u8(std::uint8_t v) { bytes_.push_back(static_cast<char>(v)); }
    void u32(std::uint32_t v);
    void u64(std::uint64_t v);
    void raw(std::string_view v) { bytes_.append(v); }
    void string(std::string_view v);
    const std::string &bytes() const { return bytes_; }
    std::string take() { return std::move(bytes_); }

  private:
    std::string bytes_;
};


class Decoder {
  public:
    explicit Decoder(std::string_view bytes) : bytes_(bytes) {}
    std::uint8_t u8();
    std::uint32_t u32();
    std::uint64_t u64();
    std::string_view raw(std::size_t n);
    std::string string(std::size_t maximum = 64 * 1024 * 1024);
    std::size_t remaining() const { return bytes_.size() - position_; }
    void finish() const { require(remaining() == 0, Code::corruption, "trailing bytes"); }

  private:
    std::string_view bytes_;
    std::size_t position_{};
};

std::uint32_t crc32c(std::string_view bytes) noexcept;

void encodeRecord(Encoder &, const Record &);
Record decodeRecord(Decoder &);

std::string checkedPayload(std::string_view payload);
std::string_view verifyPayload(std::string_view bytes);

std::string fileName(std::uint64_t id, std::string_view extension);
bool numberedFileName(std::string_view name, std::string_view extension) noexcept;

} // namespace aegisdb::detail

