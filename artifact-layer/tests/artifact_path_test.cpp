#include "encoding/artifact_path.h"

#include <array>
#include <cstdint>
#include <string>

#include "gtest/gtest.h"

namespace artifact_system::testing {

TEST(ArtifactPathTest, BuildsArtifactPathWithBigEndianBase64UrlId) {
  const uint64_t artifact_id = 0x0123456789ABCDEFULL;
  const std::string path = encoding::ArtifactPath(artifact_id);

  EXPECT_EQ(path, "artifacts/ASNFZ4mrze8");
}

TEST(ArtifactPathTest, BuildsIndexPathWithExpectedSegments) {
  const uint64_t index_definition_id = 0x0102030405060708ULL;
  const std::array<uint8_t, 4> encoded_key = {0xDE, 0xAD, 0xBE, 0xEF};

  const std::string path = encoding::IndexPath(index_definition_id, encoded_key);

  EXPECT_EQ(path, "indexes/AQIDBAUGBwg/X3jDMnTkP6neVlkmXB2RfiXANyLcsLjSfbjV_qqBOVM");
  EXPECT_EQ(path.size(), 63U);
}

TEST(ArtifactPathTest, IndexPathHashSegmentHasFixedLengthForEmptyKey) {
  const std::array<uint8_t, 0> encoded_key = {};
  const std::string path = encoding::IndexPath(42, encoded_key);

  const size_t slash = path.find_last_of('/');
  ASSERT_NE(slash, std::string::npos);
  EXPECT_EQ(path.substr(0, slash + 1), "indexes/AAAAAAAAACo/");
  EXPECT_EQ(path.substr(slash + 1).size(), 43U);
}

} // namespace artifact_system::testing
