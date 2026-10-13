#include "storage/bloom.h"
#include <algorithm>
#include <cmath>

namespace aegisdb::detail {
BloomFilter::Hash BloomFilter::hash(std::string_view key) noexcept {
    auto fnv = [key](std::uint64_t seed) {
        auto h = seed;
        for (char c : key) {
            h ^= static_cast<unsigned char>(c);
            h *= 1099511628211ULL;
        }
        h ^= h >> 33;
        h *= 0xff51afd7ed558ccdULL;
        h ^= h >> 33;
        h *= 0xc4ceb9fe1a85ec53ULL;
        return h ^ (h >> 33);
    };
    return {fnv(14695981039346656037ULL), fnv(7809847782465536322ULL) | 1ULL};
}
BloomFilter BloomFilter::build(const std::vector<Hash> &keys, double rate) {
    require(std::isfinite(rate) && rate > 0 && rate < 1, Code::invalid_argument,
            "invalid Bloom probability");
    BloomFilter f;
    if (keys.empty())
        return f;
    const auto ideal =
        std::ceil(-static_cast<double>(keys.size()) * std::log(rate) / (std::log(2) * std::log(2)));
    require(ideal <= 256 * 1024 * 1024, Code::resource_exhausted,
            "Bloom filter exceeds format bound");
    f.bits_ = std::max<std::uint64_t>(64, static_cast<std::uint64_t>(ideal));
    f.hashes_ = static_cast<std::uint32_t>(std::clamp(
        std::round(static_cast<double>(f.bits_) / static_cast<double>(keys.size()) * std::log(2)),
        1.0, 30.0));
    f.bytes_.resize(static_cast<std::size_t>((f.bits_ + 7) / 8));
    for (auto [a, b] : keys)
        for (std::uint32_t i = 0; i < f.hashes_; ++i) {
            const auto bit = (a + std::uint64_t(i) * b) % f.bits_;
            f.bytes_[bit / 8] |= static_cast<std::uint8_t>(1U << (bit % 8));
        }
    return f;
}
bool BloomFilter::mayContain(std::string_view key) const noexcept {
    if (bits_ == 0)
        return false;
    const auto [a, b] = hash(key);
    for (std::uint32_t i = 0; i < hashes_; ++i) {
        const auto bit = (a + std::uint64_t(i) * b) % bits_;
        if ((bytes_[bit / 8] & (1U << (bit % 8))) == 0)
            return false;
    }
    return true;
}
std::string BloomFilter::encode() const {
    Encoder e;
    e.u32(1);
    e.u32(1);
    e.u64(bits_);
    e.u32(hashes_);
    for (auto b : bytes_)
        e.u8(b);
    return e.take();
}
BloomFilter BloomFilter::decode(std::string_view bytes) {
    Decoder d(bytes);
    require(d.u32() == 1 && d.u32() == 1, Code::corruption, "unsupported Bloom policy");
    BloomFilter f;
    f.bits_ = d.u64();
    f.hashes_ = d.u32();
    require(f.bits_ <= 256 * 1024 * 1024 && ((f.bits_ == 0 && f.hashes_ == 0) ||
                                             (f.bits_ >= 64 && f.hashes_ >= 1 && f.hashes_ <= 30)),
            Code::corruption, "invalid Bloom dimensions");
    require(d.remaining() == (f.bits_ + 7) / 8, Code::corruption, "Bloom length mismatch");
    f.bytes_.reserve(d.remaining());
    while (d.remaining())
        f.bytes_.push_back(d.u8());
    return f;
}
} // namespace aegisdb::detail
