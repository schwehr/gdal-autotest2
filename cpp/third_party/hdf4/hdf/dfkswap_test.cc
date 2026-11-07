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

class DfkswapTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }
  void TearDown() override { HEclear(); }
};

// -----------------------------------------------------------------------------
// DFKsb2b Tests
// -----------------------------------------------------------------------------

TEST_F(DfkswapTest, Sb2bZeroElementsFails) {
  uint16_t src = 0x1234;
  uint16_t dst = 0;
  EXPECT_EQ(FAIL, DFKsb2b(&src, &dst, 0, 0, 0));
}

TEST_F(DfkswapTest, Sb2bFastProcessingOutOfPlace) {
  const uint8_t src[] = {0x12, 0x34, 0x56, 0x78};
  uint8_t dst[4] = {};

  EXPECT_EQ(0, DFKsb2b(const_cast<uint8_t*>(src), dst, 2, 0, 0));
  EXPECT_EQ(0x34, dst[0]);
  EXPECT_EQ(0x12, dst[1]);
  EXPECT_EQ(0x78, dst[2]);
  EXPECT_EQ(0x56, dst[3]);
}

TEST_F(DfkswapTest, Sb2bFastProcessingInPlace) {
  uint8_t buf[] = {0xAB, 0xCD, 0x11, 0x22};
  EXPECT_EQ(0, DFKsb2b(buf, buf, 2, 0, 0));
  EXPECT_EQ(0xCD, buf[0]);
  EXPECT_EQ(0xAB, buf[1]);
  EXPECT_EQ(0x22, buf[2]);
  EXPECT_EQ(0x11, buf[3]);
}

TEST_F(DfkswapTest, Sb2bStridedProcessingOutOfPlace) {
  // Source with stride 4: items at index 0 and 4
  uint8_t src[12] = {};
  src[0] = 0x12;
  src[1] = 0x34;
  src[4] = 0x56;
  src[5] = 0x78;

  // Dest with stride 6: items at index 0 and 6
  uint8_t dst[12] = {};
  EXPECT_EQ(0, DFKsb2b(src, dst, 2, 4, 6));
  EXPECT_EQ(0x34, dst[0]);
  EXPECT_EQ(0x12, dst[1]);
  EXPECT_EQ(0x78, dst[6]);
  EXPECT_EQ(0x56, dst[7]);
}

TEST_F(DfkswapTest, Sb2bStridedProcessingInPlace) {
  uint8_t buf[12] = {};
  buf[0] = 0xAA;
  buf[1] = 0xBB;
  buf[4] = 0xCC;
  buf[5] = 0xDD;

  EXPECT_EQ(0, DFKsb2b(buf, buf, 2, 4, 4));
  EXPECT_EQ(0xBB, buf[0]);
  EXPECT_EQ(0xAA, buf[1]);
  EXPECT_EQ(0xDD, buf[4]);
  EXPECT_EQ(0xCC, buf[5]);
}

// -----------------------------------------------------------------------------
// DFKsb4b Tests
// -----------------------------------------------------------------------------

TEST_F(DfkswapTest, Sb4bZeroElementsFails) {
  uint32_t src = 0x12345678;
  uint32_t dst = 0;
  EXPECT_EQ(FAIL, DFKsb4b(&src, &dst, 0, 0, 0));
}

TEST_F(DfkswapTest, Sb4bFastProcessingOutOfPlace) {
  const uint8_t src[] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t dst[8] = {};

  EXPECT_EQ(0, DFKsb4b(const_cast<uint8_t*>(src), dst, 2, 0, 0));
  EXPECT_EQ(4, dst[0]);
  EXPECT_EQ(3, dst[1]);
  EXPECT_EQ(2, dst[2]);
  EXPECT_EQ(1, dst[3]);
  EXPECT_EQ(8, dst[4]);
  EXPECT_EQ(7, dst[5]);
  EXPECT_EQ(6, dst[6]);
  EXPECT_EQ(5, dst[7]);
}

TEST_F(DfkswapTest, Sb4bFastProcessingInPlace) {
  uint8_t buf[] = {0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88};
  EXPECT_EQ(0, DFKsb4b(buf, buf, 2, 0, 0));
  EXPECT_EQ(0x44, buf[0]);
  EXPECT_EQ(0x33, buf[1]);
  EXPECT_EQ(0x22, buf[2]);
  EXPECT_EQ(0x11, buf[3]);
  EXPECT_EQ(0x88, buf[4]);
  EXPECT_EQ(0x77, buf[5]);
  EXPECT_EQ(0x66, buf[6]);
  EXPECT_EQ(0x55, buf[7]);
}

TEST_F(DfkswapTest, Sb4bStridedProcessingOutOfPlace) {
  uint8_t src[24] = {};
  src[0] = 1;
  src[1] = 2;
  src[2] = 3;
  src[3] = 4;
  src[8] = 5;
  src[9] = 6;
  src[10] = 7;
  src[11] = 8;

  uint8_t dst[24] = {};
  EXPECT_EQ(0, DFKsb4b(src, dst, 2, 8, 12));
  EXPECT_EQ(4, dst[0]);
  EXPECT_EQ(3, dst[1]);
  EXPECT_EQ(2, dst[2]);
  EXPECT_EQ(1, dst[3]);
  EXPECT_EQ(8, dst[12]);
  EXPECT_EQ(7, dst[13]);
  EXPECT_EQ(6, dst[14]);
  EXPECT_EQ(5, dst[15]);
}

TEST_F(DfkswapTest, Sb4bStridedProcessingInPlace) {
  uint8_t buf[24] = {};
  buf[0] = 1;
  buf[1] = 2;
  buf[2] = 3;
  buf[3] = 4;
  buf[8] = 5;
  buf[9] = 6;
  buf[10] = 7;
  buf[11] = 8;

  EXPECT_EQ(0, DFKsb4b(buf, buf, 2, 8, 8));
  EXPECT_EQ(4, buf[0]);
  EXPECT_EQ(3, buf[1]);
  EXPECT_EQ(2, buf[2]);
  EXPECT_EQ(1, buf[3]);
  EXPECT_EQ(8, buf[8]);
  EXPECT_EQ(7, buf[9]);
  EXPECT_EQ(6, buf[10]);
  EXPECT_EQ(5, buf[11]);
}

// -----------------------------------------------------------------------------
// DFKsb8b Tests
// -----------------------------------------------------------------------------

TEST_F(DfkswapTest, Sb8bZeroElementsFails) {
  uint64_t src = 0x0123456789ABCDEFULL;
  uint64_t dst = 0;
  EXPECT_EQ(FAIL, DFKsb8b(&src, &dst, 0, 0, 0));
}

TEST_F(DfkswapTest, Sb8bFastProcessingOutOfPlace) {
  const uint8_t src[] = {1, 2, 3, 4, 5, 6, 7, 8};
  uint8_t dst[8] = {};

  EXPECT_EQ(0, DFKsb8b(const_cast<uint8_t*>(src), dst, 1, 0, 0));
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(8 - i, dst[i]);
  }
}

TEST_F(DfkswapTest, Sb8bFastProcessingInPlace) {
  uint8_t buf[] = {10, 20, 30, 40, 50, 60, 70, 80};
  EXPECT_EQ(0, DFKsb8b(buf, buf, 1, 0, 0));
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ((8 - i) * 10, buf[i]);
  }
}

TEST_F(DfkswapTest, Sb8bStridedProcessingOutOfPlace) {
  uint8_t src[32] = {};
  for (int i = 0; i < 8; ++i) {
    src[i] = i + 1;
    src[16 + i] = (i + 1) * 10;
  }

  uint8_t dst[40] = {};
  EXPECT_EQ(0, DFKsb8b(src, dst, 2, 16, 20));
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(8 - i, dst[i]);
    EXPECT_EQ((8 - i) * 10, dst[20 + i]);
  }
}

TEST_F(DfkswapTest, Sb8bStridedProcessingInPlace) {
  uint8_t buf[32] = {};
  for (int i = 0; i < 8; ++i) {
    buf[i] = i + 1;
    buf[16 + i] = (i + 1) * 10;
  }

  EXPECT_EQ(0, DFKsb8b(buf, buf, 2, 16, 16));
  for (int i = 0; i < 8; ++i) {
    EXPECT_EQ(8 - i, buf[i]);
    EXPECT_EQ((8 - i) * 10, buf[16 + i]);
  }
}

}  // namespace
