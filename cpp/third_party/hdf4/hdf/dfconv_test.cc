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

#include "third_party/hdf4/hdf/src/df.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"

extern "C" {
int32 DFKqueryNT(void);
int DFKsetcustom(int (*DFKcustin)(void* source, void* dest, uint32 num_elm,
                                  uint32 source_stride, uint32 dest_stride),
                 int (*DFKcustout)(void* source, void* dest, uint32 num_elm,
                                   uint32 source_stride, uint32 dest_stride));
}

namespace {

int MockCustomIn(void* source, void* dest, uint32 num_elm, uint32 source_stride,
                 uint32 dest_stride) {
  auto* src = static_cast<uint8_t*>(source);
  auto* dst = static_cast<uint8_t*>(dest);
  for (uint32 i = 0; i < num_elm; ++i) {
    *dst = *src + 1;
    src += (source_stride == 0 ? 1 : source_stride);
    dst += (dest_stride == 0 ? 1 : dest_stride);
  }
  return 0;
}

int MockCustomOut(void* source, void* dest, uint32 num_elm,
                  uint32 source_stride, uint32 dest_stride) {
  auto* src = static_cast<uint8_t*>(source);
  auto* dst = static_cast<uint8_t*>(dest);
  for (uint32 i = 0; i < num_elm; ++i) {
    *dst = *src + 2;
    src += (source_stride == 0 ? 1 : source_stride);
    dst += (dest_stride == 0 ? 1 : dest_stride);
  }
  return 0;
}

class DfconvTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }
  void TearDown() override {
    // Reset number type to default
    DFKsetNT(DFNT_INT32);
    HEclear();
  }
};

TEST_F(DfconvTest, QueryAndSetNumberType) {
  EXPECT_EQ(0, DFKsetNT(DFNT_INT32));
  EXPECT_EQ(DFNT_INT32, DFKqueryNT());

  // Test setting each valid category of number types
  const int32 kTypes[] = {
      DFNT_CHAR8,   DFNT_UCHAR8, DFNT_INT8,    DFNT_UINT8,    DFNT_INT16,
      DFNT_UINT16,  DFNT_INT32,  DFNT_UINT32,  DFNT_FLOAT32,  DFNT_FLOAT64,
      DFNT_NCHAR,   DFNT_NINT8,  DFNT_NUCHAR,  DFNT_NUINT8,   DFNT_NINT16,
      DFNT_NUINT16, DFNT_NINT32, DFNT_NUINT32, DFNT_NFLOAT32, DFNT_NFLOAT64,
      DFNT_LCHAR,   DFNT_LINT8,  DFNT_LUCHAR,  DFNT_LUINT8,   DFNT_LINT16,
      DFNT_LUINT16, DFNT_LINT32, DFNT_LUINT32, DFNT_LFLOAT32, DFNT_LFLOAT64,
      DFNT_CUSTOM,
  };

  for (int32 nt : kTypes) {
    EXPECT_EQ(0, DFKsetNT(nt)) << "Failed for nt: " << nt;
    EXPECT_EQ(nt, DFKqueryNT());
  }

  // Invalid number type
  EXPECT_EQ(FAIL, DFKsetNT(9999));
}

TEST_F(DfconvTest, NumberTypeSize) {
  // Native types
  EXPECT_EQ(SIZE_NUCHAR, DFKNTsize(DFNT_NUCHAR));
  EXPECT_EQ(SIZE_NCHAR, DFKNTsize(DFNT_NCHAR));
  EXPECT_EQ(SIZE_NINT8, DFKNTsize(DFNT_NINT8));
  EXPECT_EQ(SIZE_NUINT8, DFKNTsize(DFNT_NUINT8));
  EXPECT_EQ(SIZE_NINT16, DFKNTsize(DFNT_NINT16));
  EXPECT_EQ(SIZE_NUINT16, DFKNTsize(DFNT_NUINT16));
  EXPECT_EQ(SIZE_NINT32, DFKNTsize(DFNT_NINT32));
  EXPECT_EQ(SIZE_NUINT32, DFKNTsize(DFNT_NUINT32));
  EXPECT_EQ(SIZE_NFLOAT32, DFKNTsize(DFNT_NFLOAT32));
  EXPECT_EQ(SIZE_NFLOAT64, DFKNTsize(DFNT_NFLOAT64));

  // HDF types
  EXPECT_EQ(SIZE_UCHAR, DFKNTsize(DFNT_UCHAR));
  EXPECT_EQ(SIZE_CHAR, DFKNTsize(DFNT_CHAR));
  EXPECT_EQ(SIZE_INT8, DFKNTsize(DFNT_INT8));
  EXPECT_EQ(SIZE_UINT8, DFKNTsize(DFNT_UINT8));
  EXPECT_EQ(SIZE_INT16, DFKNTsize(DFNT_INT16));
  EXPECT_EQ(SIZE_UINT16, DFKNTsize(DFNT_UINT16));
  EXPECT_EQ(SIZE_INT32, DFKNTsize(DFNT_INT32));
  EXPECT_EQ(SIZE_UINT32, DFKNTsize(DFNT_UINT32));
  EXPECT_EQ(SIZE_FLOAT32, DFKNTsize(DFNT_FLOAT32));
  EXPECT_EQ(SIZE_FLOAT64, DFKNTsize(DFNT_FLOAT64));

  // Little-endian bit ignored for sizing
  EXPECT_EQ(SIZE_INT16, DFKNTsize(DFNT_INT16 | DFNT_LITEND));

  // Unknown type returns FAIL
  EXPECT_EQ(FAIL, DFKNTsize(9999));
}

TEST_F(DfconvTest, IsNativeAndIsLitEnd) {
  EXPECT_EQ(1, DFKisnativeNT(DFNT_NFLOAT32));
  EXPECT_EQ(0, DFKisnativeNT(DFNT_FLOAT32));

  EXPECT_EQ(1, DFKislitendNT(DFNT_LFLOAT32));
  EXPECT_EQ(0, DFKislitendNT(DFNT_FLOAT32));
}

TEST_F(DfconvTest, PlatformNumberSubclass) {
  constexpr int32 kMachineType = 0x4321;

  // Char types: low nibble (0x1)
  EXPECT_EQ(0x1, DFKgetPNSC(DFNT_CHAR8, kMachineType));
  EXPECT_EQ(0x1, DFKgetPNSC(DFNT_UCHAR8, kMachineType));

  // Int types: second nibble (0x2)
  EXPECT_EQ(0x2, DFKgetPNSC(DFNT_INT8, kMachineType));
  EXPECT_EQ(0x2, DFKgetPNSC(DFNT_UINT8, kMachineType));
  EXPECT_EQ(0x2, DFKgetPNSC(DFNT_INT16, kMachineType));
  EXPECT_EQ(0x2, DFKgetPNSC(DFNT_UINT16, kMachineType));
  EXPECT_EQ(0x2, DFKgetPNSC(DFNT_INT32, kMachineType));
  EXPECT_EQ(0x2, DFKgetPNSC(DFNT_UINT32, kMachineType));

  // Float32: third nibble (0x3)
  EXPECT_EQ(0x3, DFKgetPNSC(DFNT_FLOAT32, kMachineType));

  // Float64: fourth nibble (0x4)
  EXPECT_EQ(0x4, DFKgetPNSC(DFNT_FLOAT64, kMachineType));

  // Invalid number type
  EXPECT_EQ(FAIL, DFKgetPNSC(0x0EEE, kMachineType));
}

TEST_F(DfconvTest, CustomConversion) {
  EXPECT_EQ(0, DFKsetcustom(MockCustomIn, MockCustomOut));
  EXPECT_EQ(DFNT_CUSTOM, DFKqueryNT());

  uint8_t src[] = {10, 20};
  uint8_t dst[2] = {};

  // READ mode calls MockCustomIn (+1)
  EXPECT_EQ(0, DFKconvert(src, dst, DFNT_CUSTOM, 2, DFACC_READ, 0, 0));
  EXPECT_EQ(11, dst[0]);
  EXPECT_EQ(21, dst[1]);

  // WRITE mode calls MockCustomOut (+2)
  EXPECT_EQ(0, DFKconvert(src, dst, DFNT_CUSTOM, 2, DFACC_WRITE, 0, 0));
  EXPECT_EQ(12, dst[0]);
  EXPECT_EQ(22, dst[1]);
}

TEST_F(DfconvTest, ConvertFunction) {
  uint32_t src_val = 0x12345678;
  uint32_t dst_val = 0;

  // Same source and dest type -> simple memcpy
  EXPECT_EQ(0, DFconvert(reinterpret_cast<uint8_t*>(&src_val),
                         reinterpret_cast<uint8_t*>(&dst_val), DFNT_INT32,
                         DFNTF_IEEE, DFNTF_IEEE, 4));
  EXPECT_EQ(src_val, dst_val);

  // IEEE to PC (little-endian byte swap)
  dst_val = 0;
  EXPECT_EQ(0, DFconvert(reinterpret_cast<uint8_t*>(&src_val),
                         reinterpret_cast<uint8_t*>(&dst_val), DFNT_INT32,
                         DFNTF_IEEE, DFNTF_PC, 4));
  EXPECT_EQ(0x78563412, dst_val);

  // PC to IEEE
  uint32_t back_val = 0;
  EXPECT_EQ(0, DFconvert(reinterpret_cast<uint8_t*>(&dst_val),
                         reinterpret_cast<uint8_t*>(&back_val), DFNT_INT32,
                         DFNTF_PC, DFNTF_IEEE, 4));
  EXPECT_EQ(src_val, back_val);

  // Invalid ntype
  EXPECT_EQ(FAIL, DFconvert(reinterpret_cast<uint8_t*>(&src_val),
                            reinterpret_cast<uint8_t*>(&dst_val), 9999,
                            DFNTF_IEEE, DFNTF_PC, 4));

  // Invalid conversion pair
  EXPECT_EQ(FAIL, DFconvert(reinterpret_cast<uint8_t*>(&src_val),
                            reinterpret_cast<uint8_t*>(&dst_val), DFNT_INT32,
                            DFNTF_PC, DFNTF_VAX, 4));
}

TEST_F(DfconvTest, DfkConvertArgumentsAndError) {
  uint8_t buf[4] = {};
  EXPECT_EQ(-1, DFKconvert(nullptr, buf, DFNT_INT32, 1, DFACC_READ, 0, 0));
  EXPECT_EQ(-1, DFKconvert(buf, nullptr, DFNT_INT32, 1, DFACC_READ, 0, 0));

  // Native conversion in READ mode
  uint32_t src = 0x12345678;
  uint32_t dst = 0;
  EXPECT_EQ(0, DFKconvert(&src, &dst, DFNT_NINT32, 1, DFACC_READ, 0, 0));
  EXPECT_EQ(src, dst);

  // Native conversion in WRITE mode
  dst = 0;
  EXPECT_EQ(0, DFKconvert(&src, &dst, DFNT_NINT32, 1, DFACC_WRITE, 0, 0));
  EXPECT_EQ(src, dst);
}

}  // namespace
