#pragma once
#include <aegisdb/status.h>
#include <string_view>

namespace aegisdb::server {
inline std::string base64Encode(std::string_view input) {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((input.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const auto a = static_cast<unsigned char>(input[i]);
        const auto b = i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0;
        const auto c = i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0;
        out.push_back(alphabet[a >> 2]);
        out.push_back(alphabet[((a & 3) << 4) | (b >> 4)]);
        out.push_back(i + 1 < input.size() ? alphabet[((b & 15) << 2) | (c >> 6)] : '=');
        out.push_back(i + 2 < input.size() ? alphabet[c & 63] : '=');
    }
    return out;
}
inline std::string base64Decode(std::string_view input, std::size_t maximum) {
    using detail::require;
    require(input.size() % 4 == 0 && input.size() <= (maximum + 2) / 3 * 4, Code::invalid_argument,
            "invalid Base64 length");
    std::string out;
    out.reserve(input.size() / 4 * 3);
    const auto digit = [](char c) {
        if (c >= 'A' && c <= 'Z')
            return c - 'A';
        if (c >= 'a' && c <= 'z')
            return c - 'a' + 26;
        if (c >= '0' && c <= '9')
            return c - '0' + 52;
        if (c == '+')
            return 62;
        if (c == '/')
            return 63;
        return -1;
    };
    for (std::size_t i = 0; i < input.size(); i += 4) {
        const auto a = digit(input[i]), b = digit(input[i + 1]);
        const bool p2 = input[i + 2] == '=', p3 = input[i + 3] == '=';
        const auto c = p2 ? 0 : digit(input[i + 2]), d = p3 ? 0 : digit(input[i + 3]);
        require(a >= 0 && b >= 0 && c >= 0 && d >= 0 && (!p2 || p3) &&
                    (!(p2 || p3) || i + 4 == input.size()),
                Code::invalid_argument, "invalid Base64 alphabet or padding");
        require((!p2 || (b & 15) == 0) && (!p3 || p2 || (c & 3) == 0), Code::invalid_argument,
                "noncanonical Base64 padding");
        out.push_back(static_cast<char>((a << 2) | (b >> 4)));
        if (!p2)
            out.push_back(static_cast<char>((b << 4) | (c >> 2)));
        if (!p3)
            out.push_back(static_cast<char>((c << 6) | d));
    }
    require(out.size() <= maximum, Code::invalid_argument, "decoded field too large");
    return out;
}
} // namespace aegisdb::server
