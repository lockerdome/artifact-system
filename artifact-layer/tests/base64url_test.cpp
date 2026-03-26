#include "encoding/base64url.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "gtest/gtest.h"

namespace artifact_system::testing {

TEST(Base64UrlTest, KnownAnswerVectors) {
  struct TestCase {
    std::vector<uint8_t> input;
    std::string expected;
  };

  const std::vector<TestCase> test_cases = {
      {{}, ""},
      {{'f'}, "Zg"},
      {{'f', 'o'}, "Zm8"},
      {{'f', 'o', 'o'}, "Zm9v"},
      {{'f', 'o', 'o', 'b'}, "Zm9vYg"},
      {{'f', 'o', 'o', 'b', 'a'}, "Zm9vYmE"},
      {{'f', 'o', 'o', 'b', 'a', 'r'}, "Zm9vYmFy"},
  };

  for (const auto& test_case : test_cases) {
    EXPECT_EQ(encoding::base64url::Encode(test_case.input), test_case.expected);
  }
}

TEST(Base64UrlTest, EncodesBigEndianUint64To11Chars) {
  const std::array<uint8_t, 8> bytes = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
  const std::string encoded = encoding::base64url::Encode(bytes);

  EXPECT_EQ(encoded, "ASNFZ4mrze8");
  EXPECT_EQ(encoded.size(), 11U);
}

TEST(Base64UrlTest, UsesUrlSafeAlphabetWithoutPadding) {
  const std::vector<uint8_t> slash_bytes = {0xFF, 0xFF, 0xFF};
  const std::vector<uint8_t> plus_bytes = {0xFB};

  const std::string slash_encoded = encoding::base64url::Encode(slash_bytes);
  const std::string plus_encoded = encoding::base64url::Encode(plus_bytes);

  EXPECT_EQ(slash_encoded, "____");
  EXPECT_EQ(plus_encoded, "-w");
  EXPECT_EQ(slash_encoded.find('='), std::string::npos);
  EXPECT_EQ(plus_encoded.find('='), std::string::npos);
}

} // namespace artifact_system::testing
