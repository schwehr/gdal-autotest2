#include <cstdint>
#include <cstring>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"

#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"

namespace {

class DfknatTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }
  void TearDown() override { HEclear(); }
};

// -----------------------------------------------------------------------------
// DFKnb1b Tests
// -----------------------------------------------------------------------------

TEST_F(DfknatTest, Nb1bZeroElementsFails) {
  uint8_t src = 1;
  uint8_t dst = 0;
  EXPECT_EQ(FAIL, DFKnb1b(&src, &dst, 0, 0, 0));
}

TEST_F(DfknatTest, Nb1bFastProcessingOutOfPlace) {
  const uint8_t src[] = {1, 2, 3, 4, 5};
  uint8_t dst[5] = {};

  // Stride (0, 0)
  EXPECT_EQ(0, DFKnb1b(const_cast<uint8_t*>(src), dst, 5, 0, 0));
  EXPECT_EQ(0, std::memcmp(src, dst, 5));

  // Stride (1, 1)
  std::memset(dst, 0, sizeof(dst));
  EXPECT_EQ(0, DFKnb1b(const_cast<uint8_t*>(src), dst, 5, 1, 1));
  EXPECT_EQ(0, std::memcmp(src, dst, 5));
}

TEST_F(DfknatTest, Nb1bFastProcessingInPlace) {
  uint8_t buf[] = {1, 2, 3, 4, 5};
  EXPECT_EQ(0, DFKnb1b(buf, buf, 5, 0, 0));
  EXPECT_EQ(1, buf[0]);
  EXPECT_EQ(0, DFKnb1b(buf, buf, 5, 1, 1));
  EXPECT_EQ(5, buf[4]);
}

TEST_F(DfknatTest, Nb1bStridedProcessing) {
  // Source with stride 2: elements at indices 0, 2, 4 -> {10, 30, 50}
  uint8_t src[] = {10, 99, 30, 99, 50};
  // Dest with stride 3
  uint8_t dst[9] = {};

  EXPECT_EQ(0, DFKnb1b(src, dst, 3, 2, 3));
  EXPECT_EQ(10, dst[0]);
  EXPECT_EQ(30, dst[3]);
  EXPECT_EQ(50, dst[6]);
}

// -----------------------------------------------------------------------------
// DFKnb2b Tests
// -----------------------------------------------------------------------------

TEST_F(DfknatTest, Nb2bZeroElementsFails) {
  uint16_t src = 1;
  uint16_t dst = 0;
  EXPECT_EQ(FAIL, DFKnb2b(&src, &dst, 0, 0, 0));
}

TEST_F(DfknatTest, Nb2bFastProcessingOutOfPlace) {
  const uint16_t src[] = {0x0102, 0x0304, 0x0506};
  uint16_t dst[3] = {};

  // Stride (0, 0)
  EXPECT_EQ(0, DFKnb2b(const_cast<uint16_t*>(src), dst, 3, 0, 0));
  EXPECT_EQ(0, std::memcmp(src, dst, sizeof(src)));

  // Stride (2, 2)
  std::memset(dst, 0, sizeof(dst));
  EXPECT_EQ(0, DFKnb2b(const_cast<uint16_t*>(src), dst, 3, 2, 2));
  EXPECT_EQ(0, std::memcmp(src, dst, sizeof(src)));
}

TEST_F(DfknatTest, Nb2bFastProcessingInPlace) {
  uint16_t buf[] = {0x1122, 0x3344};
  EXPECT_EQ(0, DFKnb2b(buf, buf, 2, 0, 0));
  EXPECT_EQ(0x1122, buf[0]);
  EXPECT_EQ(0, DFKnb2b(buf, buf, 2, 2, 2));
  EXPECT_EQ(0x3344, buf[1]);
}

TEST_F(DfknatTest, Nb2bStridedProcessingOutOfPlace) {
  uint8_t src[12] = {};
  // Place two 2-byte items at stride 4: index 0 and index 4
  src[0] = 0xAA;
  src[1] = 0xBB;
  src[4] = 0xCC;
  src[5] = 0xDD;

  uint8_t dst[12] = {};
  // Dest stride 6: placed at index 0 and index 6
  EXPECT_EQ(0, DFKnb2b(src, dst, 2, 4, 6));
  EXPECT_EQ(0xAA, dst[0]);
  EXPECT_EQ(0xBB, dst[1]);
  EXPECT_EQ(0xCC, dst[6]);
  EXPECT_EQ(0xDD, dst[7]);
}

TEST_F(DfknatTest, Nb2bStridedProcessingInPlace) {
  uint8_t buf[12] = {};
  buf[0] = 0x11;
  buf[1] = 0x22;
  buf[4] = 0x33;
  buf[4 + 1] = 0x44;

  EXPECT_EQ(0, DFKnb2b(buf, buf, 2, 4, 4));
  EXPECT_EQ(0x11, buf[0]);
  EXPECT_EQ(0x22, buf[1]);
  EXPECT_EQ(0x33, buf[4]);
  EXPECT_EQ(0x44, buf[5]);
}

// -----------------------------------------------------------------------------
// DFKnb4b Tests
// -----------------------------------------------------------------------------

TEST_F(DfknatTest, Nb4bZeroElementsFails) {
  uint32_t src = 1;
  uint32_t dst = 0;
  EXPECT_EQ(FAIL, DFKnb4b(&src, &dst, 0, 0, 0));
}

TEST_F(DfknatTest, Nb4bFastProcessingOutOfPlace) {
  const uint32_t src[] = {0x11223344, 0x55667788};
  uint32_t dst[2] = {};

  // Stride (0, 0)
  EXPECT_EQ(0, DFKnb4b(const_cast<uint32_t*>(src), dst, 2, 0, 0));
  EXPECT_EQ(0, std::memcmp(src, dst, sizeof(src)));

  // Stride (4, 4)
  std::memset(dst, 0, sizeof(dst));
  EXPECT_EQ(0, DFKnb4b(const_cast<uint32_t*>(src), dst, 2, 4, 4));
  EXPECT_EQ(0, std::memcmp(src, dst, sizeof(src)));
}

TEST_F(DfknatTest, Nb4bFastProcessingInPlace) {
  uint32_t buf[] = {0xA1B2C3D4, 0xE5F60718};
  EXPECT_EQ(0, DFKnb4b(buf, buf, 2, 0, 0));
  EXPECT_EQ(0xA1B2C3D4, buf[0]);
  EXPECT_EQ(0, DFKnb4b(buf, buf, 2, 4, 4));
  EXPECT_EQ(0xE5F60718, buf[1]);
}

TEST_F(DfknatTest, Nb4bStridedProcessingOutOfPlace) {
  uint8_t src[20] = {};
  src[0] = 1;
  src[1] = 2;
  src[2] = 3;
  src[3] = 4;
  src[8] = 5;
  src[9] = 6;
  src[10] = 7;
  src[11] = 8;

  uint8_t dst[20] = {};
  EXPECT_EQ(0, DFKnb4b(src, dst, 2, 8, 12));
  EXPECT_EQ(1, dst[0]);
  EXPECT_EQ(4, dst[3]);
  EXPECT_EQ(5, dst[12]);
  EXPECT_EQ(8, dst[15]);
}

TEST_F(DfknatTest, Nb4bStridedProcessingInPlace) {
  uint8_t buf[20] = {};
  buf[0] = 1;
  buf[1] = 2;
  buf[2] = 3;
  buf[3] = 4;
  buf[8] = 5;
  buf[9] = 6;
  buf[10] = 7;
  buf[11] = 8;

  EXPECT_EQ(0, DFKnb4b(buf, buf, 2, 8, 8));
  EXPECT_EQ(1, buf[0]);
  EXPECT_EQ(4, buf[3]);
  EXPECT_EQ(5, buf[8]);
  EXPECT_EQ(8, buf[11]);
}

// -----------------------------------------------------------------------------
// DFKnb8b Tests
// -----------------------------------------------------------------------------

TEST_F(DfknatTest, Nb8bZeroElementsFails) {
  uint64_t src = 1;
  uint64_t dst = 0;
  EXPECT_EQ(FAIL, DFKnb8b(&src, &dst, 0, 0, 0));
}

TEST_F(DfknatTest, Nb8bFastProcessingOutOfPlace) {
  const uint64_t src[] = {0x0123456789ABCDEFULL, 0xFEDCBA9876543210ULL};
  uint64_t dst[2] = {};

  // Stride (0, 0)
  EXPECT_EQ(0, DFKnb8b(const_cast<uint64_t*>(src), dst, 2, 0, 0));
  EXPECT_EQ(0, std::memcmp(src, dst, sizeof(src)));

  // Stride (8, 8)
  std::memset(dst, 0, sizeof(dst));
  EXPECT_EQ(0, DFKnb8b(const_cast<uint64_t*>(src), dst, 2, 8, 8));
  EXPECT_EQ(0, std::memcmp(src, dst, sizeof(src)));
}

TEST_F(DfknatTest, Nb8bFastProcessingInPlace) {
  uint64_t buf[] = {0x1111222233334444ULL, 0x5555666677778888ULL};
  EXPECT_EQ(0, DFKnb8b(buf, buf, 2, 0, 0));
  EXPECT_EQ(0x1111222233334444ULL, buf[0]);
  EXPECT_EQ(0, DFKnb8b(buf, buf, 2, 8, 8));
  EXPECT_EQ(0x5555666677778888ULL, buf[1]);
}

TEST_F(DfknatTest, Nb8bStridedProcessingOutOfPlace) {
  uint8_t src[32] = {};
  for (int i = 0; i < 8; ++i) {
    src[i] = i + 1;
    src[16 + i] = i + 10;
  }

  uint8_t dst[40] = {};
  EXPECT_EQ(0, DFKnb8b(src, dst, 2, 16, 20));
  EXPECT_EQ(1, dst[0]);
  EXPECT_EQ(8, dst[7]);
  EXPECT_EQ(10, dst[20]);
  EXPECT_EQ(17, dst[27]);
}

TEST_F(DfknatTest, Nb8bStridedProcessingInPlace) {
  uint8_t buf[32] = {};
  for (int i = 0; i < 8; ++i) {
    buf[i] = i + 1;
    buf[16 + i] = i + 10;
  }

  EXPECT_EQ(0, DFKnb8b(buf, buf, 2, 16, 16));
  EXPECT_EQ(1, buf[0]);
  EXPECT_EQ(8, buf[7]);
  EXPECT_EQ(10, buf[16]);
  EXPECT_EQ(17, buf[23]);
}

}  // namespace
