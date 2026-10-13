#include "server/base64.h"
#include <gtest/gtest.h>
using namespace aegisdb;
TEST(Base64, BinaryRoundtripAndStrictDecoding) {
    std::string binary;
    for (int i = 0; i < 256; ++i)
        binary.push_back(static_cast<char>(i));
    for (std::size_t n = 0; n < binary.size(); ++n)
        EXPECT_EQ(server::base64Decode(server::base64Encode(binary.substr(0, n)), 256),
                  binary.substr(0, n));
    for (auto invalid : {"a", "abcd=", "=AAA", "A===", "AA=A", "AB==", "AAB=", "####", "AA==AAAA"})
        EXPECT_THROW(server::base64Decode(invalid, 256), detail::Error);
}
