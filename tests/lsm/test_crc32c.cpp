#include "crc32c.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

using namespace sextant::lsm;

// The canonical CRC32C check value. If this passes, the polynomial, the
// reflection and the pre/post inversion are all right.
TEST(CRC32C, StandardCheckVector) {
  const std::string check = "123456789";
  EXPECT_EQ(0xE3069283u, crc32c::Value(check.data(), check.size()));
}

TEST(CRC32C, EmptyInputIsZero) {
  EXPECT_EQ(0u, crc32c::Value("", 0));
}

TEST(CRC32C, DetectsSingleBitFlip) {
  std::string data(1024, '\0');
  for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 7);

  const uint32_t original = crc32c::Value(data.data(), data.size());

  // Every single-bit flip must change the CRC. This is the property the WAL
  // relies on to detect a torn or corrupted record.
  for (size_t byte = 0; byte < data.size(); byte += 37) {
    for (int bit = 0; bit < 8; ++bit) {
      std::string corrupted = data;
      corrupted[byte] = static_cast<char>(corrupted[byte] ^ (1 << bit));
      EXPECT_NE(original, crc32c::Value(corrupted.data(), corrupted.size()))
          << "byte=" << byte << " bit=" << bit;
    }
  }
}

TEST(CRC32C, ExtendIsIncremental) {
  const std::string whole = "hello world, this is a record payload";
  const uint32_t one_shot = crc32c::Value(whole.data(), whole.size());

  // Computing in pieces must equal computing in one go - the WAL writer relies
  // on this to CRC the type byte and payload separately.
  for (size_t split = 0; split <= whole.size(); ++split) {
    uint32_t c = crc32c::Extend(0, whole.data(), split);
    c = crc32c::Extend(c, whole.data() + split, whole.size() - split);
    EXPECT_EQ(one_shot, c) << "split=" << split;
  }
}

TEST(CRC32C, MaskUnmaskRoundTrip) {
  for (uint32_t crc : {0u, 1u, 0xE3069283u, 0xFFFFFFFFu, 0x80000000u}) {
    EXPECT_EQ(crc, crc32c::Unmask(crc32c::Mask(crc)));
  }
}

// Masking exists so a stored CRC can never be confused with a live one. Verify
// the mask actually changes the value.
TEST(CRC32C, MaskChangesValue) {
  const uint32_t crc = 0xE3069283u;
  EXPECT_NE(crc, crc32c::Mask(crc));
}
