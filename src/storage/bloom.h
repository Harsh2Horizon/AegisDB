#pragma once
#include "core/codec.h"
#include <utility>

namespace aegisdb::detail {
class BloomFilter {
  public:
    using Hash = std::pair<std::uint64_t, std::uint64_t>;
    static Hash hash(std::string_view) noexcept;
    static BloomFilter build(const std::vector<Hash> &distinctHashes, double falsePositiveRate);
    static BloomFilter decode(std::string_view);
    std::string encode() const;
    bool mayContain(std::string_view) const noexcept;
    std::uint64_t bitCount() const { return bits_; }

  private:
    std::vector<std::uint8_t> bytes_;
    std::uint64_t bits_{};
    std::uint32_t hashes_{};
};
} // namespace aegisdb::detail
