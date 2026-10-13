#pragma once
#include <aegisdb/status.h>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>

namespace aegisdb::detail {
using Sequence = std::uint64_t;

constexpr Sequence maxSequence = std::numeric_limits<Sequence>::max();
constexpr std::size_t encodedKeyLimit = 1024 * 1024;
constexpr std::size_t encodedValueLimit = 16 * 1024 * 1024;

enum class Kind : std::uint8_t { value = 1, deletion = 2 };

struct InternalKey {
    std::string key;
    Sequence sequence{};
};

struct Record {
    std::string key;
    Sequence sequence{};
    Kind kind{Kind::value};
    std::string value;
};

// std::string_view comparison uses char_traits' unsigned-byte lexicographic comparison.
inline int compareInternal(std::string_view a, Sequence as, std::string_view b,
                           Sequence bs) noexcept {
    const auto order = a.compare(b);
    if (order != 0)
        return order;
    return as > bs ? -1 : as < bs ? 1 : 0;
}

struct InternalLess {
    bool operator()(const Record &a, const Record &b) const noexcept {
        return compareInternal(a.key, a.sequence, b.key, b.sequence) < 0;
    }
    bool operator()(const InternalKey &a, const InternalKey &b) const noexcept {
        return compareInternal(a.key, a.sequence, b.key, b.sequence) < 0;
    }
};

inline std::size_t recordBytes(const Record &r) { return 17 + r.key.size() + r.value.size(); } // why 17 ?


} // namespace aegisdb::detail
