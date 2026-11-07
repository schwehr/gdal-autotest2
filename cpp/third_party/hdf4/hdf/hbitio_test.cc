// Copyright 2026 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Tests for HDF bit level I/O routines (hdf/src/hbitio.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/hbitio.c
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/test/bitio.c

#include <cstdint>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

#ifdef SUCCEED
#undef SUCCEED
#endif
#ifdef FAIL
#undef FAIL
#endif

#define SUCCEED 0
#define FAIL (-1)

#include "third_party/hdf4/hdf/src/hbitio.h"
#include "third_party/hdf4/hdf/src/hbitio_priv.h"
#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"
#include "third_party/hdf4/hdf/src/hfile_priv.h"

namespace hdf4 {
namespace {

std::string GetTempFilePath(const std::string& name) {
  return (std::filesystem::path(::testing::TempDir()) / name).string();
}

class HbitioTest : public ::testing::Test {
 protected:
  void SetUp() override { HEclear(); }
  void TearDown() override { HEclear(); }
};

TEST_F(HbitioTest, MaskTablesAndPutbitMacro) {
  for (int i = 0; i <= 8; ++i) {
    const uint8_t expected = (i == 0) ? 0 : static_cast<uint8_t>((1 << i) - 1);
    EXPECT_EQ(maskc[i], expected);
  }

  for (int i = 0; i <= 32; ++i) {
    const uint32_t expected = (i == 32) ? 0xFFFFFFFFU : ((1ULL << i) - 1ULL);
    EXPECT_EQ(maskl[i], expected);
  }

  // Hputbit on invalid bitid
  EXPECT_EQ(FAIL, Hputbit(-1, 1));
}

TEST_F(HbitioTest, StartBitReadArgumentValidation) {
  EXPECT_EQ(FAIL, Hstartbitread(FAIL, 100, 1));
  EXPECT_EQ(DFE_BADAID, HEvalue(1));

  const std::string path = GetTempFilePath("hbitio_read_arg.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  // Non-existent tag/ref
  EXPECT_EQ(FAIL, Hstartbitread(fid, 100, 1));
  EXPECT_EQ(DFE_BADAID, HEvalue(1));
}

TEST_F(HbitioTest, StartBitWriteArgumentValidation) {
  EXPECT_EQ(FAIL, Hstartbitwrite(FAIL, 100, 1, 0));
  EXPECT_EQ(DFE_BADAID, HEvalue(1));
}

TEST_F(HbitioTest, BitAppendableValidation) {
  EXPECT_EQ(FAIL, Hbitappendable(-1));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  const std::string path = GetTempFilePath("hbitio_appendable.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  // Create an element
  const int32_t write_bitid = Hstartbitwrite(fid, 100, 1, 0);
  ASSERT_NE(write_bitid, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(write_bitid));
  EXPECT_EQ(SUCCEED, Hendbitaccess(write_bitid, 0));

  // Open for read and check Hbitappendable -> should fail with DFE_BADACC
  const int32_t read_bitid = Hstartbitread(fid, 100, 1);
  ASSERT_NE(read_bitid, FAIL);
  EXPECT_EQ(FAIL, Hbitappendable(read_bitid));
  EXPECT_EQ(DFE_BADACC, HEvalue(1));
  EXPECT_EQ(SUCCEED, Hendbitaccess(read_bitid, 0));
}

TEST_F(HbitioTest, BitWriteArgumentValidation) {
  const std::string path = GetTempFilePath("hbitio_write_args.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  const int32_t write_bitid = Hstartbitwrite(fid, 100, 1, 0);
  ASSERT_NE(write_bitid, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(write_bitid));
  absl::Cleanup bit_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hendbitaccess(write_bitid, 0));
  };

  // count <= 0
  EXPECT_EQ(FAIL, Hbitwrite(write_bitid, 0, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  EXPECT_EQ(FAIL, Hbitwrite(write_bitid, -5, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // invalid bitid
  EXPECT_EQ(FAIL, Hbitwrite(-1, 8, 0xFF));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // write on read-only bitid
  const int32_t write_bitid2 = Hstartbitwrite(fid, 101, 1, 0);
  ASSERT_NE(write_bitid2, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(write_bitid2));
  EXPECT_EQ(8, Hbitwrite(write_bitid2, 8, 0xAB));
  EXPECT_EQ(SUCCEED, Hendbitaccess(write_bitid2, 0));

  const int32_t read_bitid = Hstartbitread(fid, 101, 1);
  ASSERT_NE(read_bitid, FAIL);
  EXPECT_EQ(FAIL, Hbitwrite(read_bitid, 8, 0xCD));
  EXPECT_EQ(DFE_BADACC, HEvalue(1));
  EXPECT_EQ(SUCCEED, Hendbitaccess(read_bitid, 0));
}

TEST_F(HbitioTest, BitReadArgumentValidation) {
  const std::string path = GetTempFilePath("hbitio_read_args.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  const int32_t write_bitid = Hstartbitwrite(fid, 100, 1, 0);
  ASSERT_NE(write_bitid, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(write_bitid));
  EXPECT_EQ(8, Hbitwrite(write_bitid, 8, 0x55));
  EXPECT_EQ(SUCCEED, Hendbitaccess(write_bitid, 0));

  const int32_t read_bitid = Hstartbitread(fid, 100, 1);
  ASSERT_NE(read_bitid, FAIL);
  absl::Cleanup bit_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hendbitaccess(read_bitid, 0));
  };

  uint32_t data = 0;
  // count <= 0
  EXPECT_EQ(FAIL, Hbitread(read_bitid, 0, &data));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
  EXPECT_EQ(FAIL, Hbitread(read_bitid, -1, &data));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // invalid bitid
  EXPECT_EQ(FAIL, Hbitread(-1, 8, &data));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
}

TEST_F(HbitioTest, BitSeekArgumentValidation) {
  const std::string path = GetTempFilePath("hbitio_seek_args.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  const int32_t write_bitid = Hstartbitwrite(fid, 100, 1, 0);
  ASSERT_NE(write_bitid, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(write_bitid));
  EXPECT_EQ(16, Hbitwrite(write_bitid, 16, 0x1234));

  EXPECT_EQ(FAIL, Hbitseek(-1, 0, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  EXPECT_EQ(FAIL, Hbitseek(write_bitid, -1, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  EXPECT_EQ(FAIL, Hbitseek(write_bitid, 0, -1));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  EXPECT_EQ(FAIL, Hbitseek(write_bitid, 0, 8));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // byte_offset > max_offset (which is 2 bytes here)
  EXPECT_EQ(FAIL, Hbitseek(write_bitid, 10, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  EXPECT_EQ(SUCCEED, Hendbitaccess(write_bitid, 0));
}

TEST_F(HbitioTest, EndBitAccessArgumentValidation) {
  EXPECT_EQ(FAIL, Hendbitaccess(-1, 0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
}

TEST_F(HbitioTest, BasicBitWriteAndRead) {
  const std::string path = GetTempFilePath("hbitio_basic.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  const int32_t write_bitid = Hstartbitwrite(fid, 100, 1, 0);
  ASSERT_NE(write_bitid, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(write_bitid));

  // Write 3 bits (value 5: 0b101)
  EXPECT_EQ(3, Hbitwrite(write_bitid, 3, 5));
  // Write 5 bits (value 17: 0b10001) -> completes 1st byte (0b10110001 = 0xB1)
  EXPECT_EQ(5, Hbitwrite(write_bitid, 5, 17));
  // Write 16 bits (0x1234) -> spans whole bytes
  EXPECT_EQ(16, Hbitwrite(write_bitid, 16, 0x1234));
  // Write 12 bits (0xABC)
  EXPECT_EQ(12, Hbitwrite(write_bitid, 12, 0xABC));
  // Write clamped count (> 32 bits, clamped to 32)
  EXPECT_EQ(40, Hbitwrite(write_bitid, 40, 0x12345678));

  // Flush with zeros
  EXPECT_EQ(SUCCEED, Hendbitaccess(write_bitid, 0));

  // Reopen for reading
  const int32_t read_bitid = Hstartbitread(fid, 100, 1);
  ASSERT_NE(read_bitid, FAIL);
  absl::Cleanup read_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hendbitaccess(read_bitid, 0));
  };

  uint32_t val = 0;
  EXPECT_EQ(3, Hbitread(read_bitid, 3, &val));
  EXPECT_EQ(val, 5U);

  EXPECT_EQ(5, Hbitread(read_bitid, 5, &val));
  EXPECT_EQ(val, 17U);

  EXPECT_EQ(16, Hbitread(read_bitid, 16, &val));
  EXPECT_EQ(val, 0x1234U);

  EXPECT_EQ(12, Hbitread(read_bitid, 12, &val));
  EXPECT_EQ(val, 0xABCU);

  // Read the 32 bits written by the clamped write
  EXPECT_EQ(32, Hbitread(read_bitid, 32, &val));
  EXPECT_EQ(val, 0x12345678U);
}

TEST_F(HbitioTest, GetBitAndPutBit) {
  const std::string path = GetTempFilePath("hbitio_getput.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  const int32_t write_bitid = Hstartbitwrite(fid, 200, 1, 0);
  ASSERT_NE(write_bitid, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(write_bitid));

  // Write sequence 1, 0, 1, 1, 0, 0, 1, 0 via Hputbit
  EXPECT_EQ(SUCCEED, Hputbit(write_bitid, 1));
  EXPECT_EQ(SUCCEED, Hputbit(write_bitid, 0));
  EXPECT_EQ(SUCCEED, Hputbit(write_bitid, 1));
  EXPECT_EQ(SUCCEED, Hputbit(write_bitid, 1));
  EXPECT_EQ(SUCCEED, Hputbit(write_bitid, 0));
  EXPECT_EQ(SUCCEED, Hputbit(write_bitid, 0));
  EXPECT_EQ(SUCCEED, Hputbit(write_bitid, 1));
  EXPECT_EQ(SUCCEED, Hputbit(write_bitid, 0));

  EXPECT_EQ(SUCCEED, Hendbitaccess(write_bitid, 0));

  const int32_t read_bitid = Hstartbitread(fid, 200, 1);
  ASSERT_NE(read_bitid, FAIL);
  absl::Cleanup read_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hendbitaccess(read_bitid, 0));
  };

  EXPECT_EQ(1, Hgetbit(read_bitid));
  EXPECT_EQ(0, Hgetbit(read_bitid));
  EXPECT_EQ(1, Hgetbit(read_bitid));
  EXPECT_EQ(1, Hgetbit(read_bitid));
  EXPECT_EQ(0, Hgetbit(read_bitid));
  EXPECT_EQ(0, Hgetbit(read_bitid));
  EXPECT_EQ(1, Hgetbit(read_bitid));
  EXPECT_EQ(0, Hgetbit(read_bitid));

  // Invalid bitid
  EXPECT_EQ(FAIL, Hgetbit(-1));
}

TEST_F(HbitioTest, LargeWriteBufferFlushAndReadRefill) {
  // BITBUF_SIZE is 4096 bytes. Writing more than 4096 bytes forces buffer
  // flush.
  const std::string path = GetTempFilePath("hbitio_large.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  const int32_t write_bitid = Hstartbitwrite(fid, 300, 1, 0);
  ASSERT_NE(write_bitid, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(write_bitid));

  // Write 1500 uint32 values (6000 bytes > 4096 BITBUF_SIZE)
  for (uint32_t i = 0; i < 1500; ++i) {
    EXPECT_EQ(32, Hbitwrite(write_bitid, 32, i));
  }
  // Write a partial byte (5 bits)
  EXPECT_EQ(5, Hbitwrite(write_bitid, 5, 0x1F));

  EXPECT_EQ(SUCCEED, Hendbitaccess(write_bitid, 0));

  // Read back and verify
  const int32_t read_bitid = Hstartbitread(fid, 300, 1);
  ASSERT_NE(read_bitid, FAIL);
  absl::Cleanup read_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hendbitaccess(read_bitid, 0));
  };

  for (uint32_t i = 0; i < 1500; ++i) {
    uint32_t val = 0;
    EXPECT_EQ(32, Hbitread(read_bitid, 32, &val));
    ASSERT_EQ(val, i) << "Mismatch at index " << i;
  }
  uint32_t last_val = 0;
  EXPECT_EQ(5, Hbitread(read_bitid, 5, &last_val));
  EXPECT_EQ(last_val, 0x1FU);
}

TEST_F(HbitioTest, MultiByteWholeAndPartialReadBufferRefill) {
  // Test reading across buffer boundary using whole bytes (BITNUM) and partial
  // bits
  const std::string path = GetTempFilePath("hbitio_refill.hdf");
  int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    if (fid != FAIL) {
      Hclose(fid);
    }
    std::filesystem::remove(path);
  };

  const int32_t write_bitid = Hstartbitwrite(fid, 400, 1, 0);
  ASSERT_NE(write_bitid, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(write_bitid));

  // Write 4095 bytes first
  std::vector<uint8_t> data(5000);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(i & 0xFF);
  }
  for (size_t i = 0; i < 4095; ++i) {
    EXPECT_EQ(8, Hbitwrite(write_bitid, 8, data[i]));
  }
  // Now write 24 bits that cross the 4096-byte boundary
  uint32_t cross = (static_cast<uint32_t>(data[4095]) << 16) |
                   (static_cast<uint32_t>(data[4096]) << 8) |
                   static_cast<uint32_t>(data[4097]);
  EXPECT_EQ(24, Hbitwrite(write_bitid, 24, cross));

  // Write remaining bytes
  for (size_t i = 4098; i < data.size(); ++i) {
    EXPECT_EQ(8, Hbitwrite(write_bitid, 8, data[i]));
  }
  EXPECT_EQ(SUCCEED, Hendbitaccess(write_bitid, 0));
  EXPECT_EQ(SUCCEED, Hclose(fid));
  fid = FAIL;

  const int32_t read_fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_NE(read_fid, FAIL);
  absl::Cleanup read_fid_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(read_fid));
    std::filesystem::remove(path);
  };

  // Now read back crossing boundary with multi-byte read
  const int32_t read_bitid = Hstartbitread(read_fid, 400, 1);
  ASSERT_NE(read_bitid, FAIL);
  absl::Cleanup read_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hendbitaccess(read_bitid, 0));
  };

  // Read 4095 bytes
  for (size_t i = 0; i < 4095; ++i) {
    uint32_t val = 0;
    EXPECT_EQ(8, Hbitread(read_bitid, 8, &val));
    ASSERT_EQ(val, data[i]);
  }
  // Read 24 bits across boundary
  uint32_t read_cross = 0;
  EXPECT_EQ(24, Hbitread(read_bitid, 24, &read_cross));
  EXPECT_EQ(read_cross, cross);

  // Read remaining
  for (size_t i = 4098; i < data.size(); ++i) {
    uint32_t val = 0;
    EXPECT_EQ(8, Hbitread(read_bitid, 8, &val));
    ASSERT_EQ(val, data[i]);
  }

  // Reading past EOF
  uint32_t eof_val = 0;
  EXPECT_EQ(0, Hbitread(read_bitid, 16, &eof_val));
  EXPECT_EQ(0, Hbitread(read_bitid, 4, &eof_val));
}

TEST_F(HbitioTest, SeekReadAndWriteVariations) {
  const std::string path = GetTempFilePath("hbitio_seek.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  const int32_t write_bitid = Hstartbitwrite(fid, 500, 1, 0);
  ASSERT_NE(write_bitid, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(write_bitid));

  // Write 6000 bytes so we span across blocks (> 4096)
  for (int i = 0; i < 6000; ++i) {
    EXPECT_EQ(8, Hbitwrite(write_bitid, 8, (i & 0xFF)));
  }

  // Seek within current block (new_block == FALSE)
  EXPECT_EQ(SUCCEED, Hbitseek(write_bitid, 5000, 0));
  // Seek with bit_offset > 0
  EXPECT_EQ(SUCCEED, Hbitseek(write_bitid, 5000, 3));

  // Seek to different block (new_block == TRUE: seek to byte 100)
  EXPECT_EQ(SUCCEED, Hbitseek(write_bitid, 100, 0));
  EXPECT_EQ(SUCCEED, Hbitseek(write_bitid, 100, 4));

  EXPECT_EQ(SUCCEED, Hendbitaccess(write_bitid, 0));

  // Read mode seeking
  const int32_t read_bitid = Hstartbitread(fid, 500, 1);
  ASSERT_NE(read_bitid, FAIL);
  absl::Cleanup read_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hendbitaccess(read_bitid, 0));
  };

  // Seek within block 0 with bit_offset == 0
  EXPECT_EQ(SUCCEED, Hbitseek(read_bitid, 50, 0));
  uint32_t val = 0;
  EXPECT_EQ(8, Hbitread(read_bitid, 8, &val));
  EXPECT_EQ(val, 50U);

  // Seek within block 0 with bit_offset > 0
  EXPECT_EQ(SUCCEED, Hbitseek(read_bitid, 50, 4));
  EXPECT_EQ(4, Hbitread(read_bitid, 4, &val));
  EXPECT_EQ(val, (50U & 0x0F));

  // Seek to block 1 (byte 4500)
  EXPECT_EQ(SUCCEED, Hbitseek(read_bitid, 4500, 0));
  EXPECT_EQ(8, Hbitread(read_bitid, 8, &val));
  EXPECT_EQ(val, (4500U & 0xFF));

  // Seek to block 1 with bit_offset > 0
  EXPECT_EQ(SUCCEED, Hbitseek(read_bitid, 4500, 2));
  EXPECT_EQ(6, Hbitread(read_bitid, 6, &val));
  EXPECT_EQ(val, (4500U & 0x3F));
}

TEST_F(HbitioTest, ModeSwitchingReadToWriteAndWriteToRead) {
  const std::string path = GetTempFilePath("hbitio_modeswitch.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  const int32_t bitid = Hstartbitwrite(fid, 600, 1, 0);
  ASSERT_NE(bitid, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(bitid));
  absl::Cleanup bit_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hendbitaccess(bitid, 0));
  };

  // Currently in write mode. Write 4 bytes.
  EXPECT_EQ(32, Hbitwrite(bitid, 32, 0x11223344));

  // Seek back to start
  EXPECT_EQ(SUCCEED, Hbitseek(bitid, 0, 0));

  // Calling Hbitread while in write mode triggers HIwrite2read!
  uint32_t val = 0;
  EXPECT_EQ(16, Hbitread(bitid, 16, &val));
  EXPECT_EQ(val, 0x1122U);

  // Now in read mode. Calling Hbitwrite triggers HIread2write!
  EXPECT_EQ(8, Hbitwrite(bitid, 8, 0xAA));

  // Seek back to byte 2
  EXPECT_EQ(SUCCEED, Hbitseek(bitid, 2, 0));
  EXPECT_EQ(8, Hbitread(bitid, 8, &val));
  EXPECT_EQ(val, 0xAAU);
}

TEST_F(HbitioTest, FlushbitVariations) {
  const std::string path = GetTempFilePath("hbitio_flush.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  // Case 1: flushbit = 1 (pad with 1s)
  {
    const int32_t bitid = Hstartbitwrite(fid, 700, 1, 0);
    ASSERT_NE(bitid, FAIL);
    EXPECT_EQ(SUCCEED, Hbitappendable(bitid));
    // Write 4 bits: 0b1010
    EXPECT_EQ(4, Hbitwrite(bitid, 4, 0xA));
    EXPECT_EQ(SUCCEED, Hendbitaccess(bitid, 1));

    const int32_t rbitid = Hstartbitread(fid, 700, 1);
    ASSERT_NE(rbitid, FAIL);
    uint32_t byte_val = 0;
    EXPECT_EQ(8, Hbitread(rbitid, 8, &byte_val));
    // 0b1010 followed by 4 ones (0b1111) = 0xAF
    EXPECT_EQ(byte_val, 0xAFU);
    EXPECT_EQ(SUCCEED, Hendbitaccess(rbitid, 0));
  }

  // Case 2: flushbit = -1 (throw away leftover bits)
  {
    const int32_t bitid = Hstartbitwrite(fid, 701, 1, 0);
    ASSERT_NE(bitid, FAIL);
    EXPECT_EQ(SUCCEED, Hbitappendable(bitid));
    // Write 1 full byte (0x55) + 3 partial bits (0b111)
    EXPECT_EQ(8, Hbitwrite(bitid, 8, 0x55));
    EXPECT_EQ(3, Hbitwrite(bitid, 3, 0x7));
    EXPECT_EQ(SUCCEED, Hendbitaccess(bitid, -1));

    const int32_t rbitid = Hstartbitread(fid, 701, 1);
    ASSERT_NE(rbitid, FAIL);
    uint32_t byte_val = 0;
    EXPECT_EQ(8, Hbitread(rbitid, 8, &byte_val));
    EXPECT_EQ(byte_val, 0x55U);
    EXPECT_EQ(SUCCEED, Hendbitaccess(rbitid, 0));
  }

  // Case 3: Flush in the middle of a dataset (else branch in HIbitflush)
  {
    const int32_t bitid = Hstartbitwrite(fid, 702, 1, 0);
    ASSERT_NE(bitid, FAIL);
    EXPECT_EQ(SUCCEED, Hbitappendable(bitid));
    // Write 4 full bytes (0x00, 0x00, 0x00, 0x00)
    for (int i = 0; i < 4; ++i) {
      EXPECT_EQ(8, Hbitwrite(bitid, 8, 0x00));
    }
    // Seek to byte 1, bit 2
    EXPECT_EQ(SUCCEED, Hbitseek(bitid, 1, 2));
    // Write 3 bits (0b101)
    EXPECT_EQ(3, Hbitwrite(bitid, 3, 5));
    // Close with Hendbitaccess -> triggers middle-of-dataset merge in
    // HIbitflush
    EXPECT_EQ(SUCCEED, Hendbitaccess(bitid, 0));

    const int32_t rbitid = Hstartbitread(fid, 702, 1);
    ASSERT_NE(rbitid, FAIL);
    uint32_t val = 0;
    EXPECT_EQ(8, Hbitread(rbitid, 8, &val));
    EXPECT_EQ(val, 0x00U);
    EXPECT_EQ(8, Hbitread(rbitid, 8, &val));
    // In byte 1: bit offset was 2, bits written 101, so bits 2..4 are 101 ->
    // 0b00101000 = 0x28
    EXPECT_EQ(val, 0x28U);
    EXPECT_EQ(SUCCEED, Hendbitaccess(rbitid, 0));
  }
}

TEST_F(HbitioTest, StartBitWriteExistingElementAndPreRead) {
  const std::string path = GetTempFilePath("hbitio_exist.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  // Create an element with 5000 bytes
  const int32_t bitid1 = Hstartbitwrite(fid, 800, 1, 0);
  ASSERT_NE(bitid1, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(bitid1));
  for (int i = 0; i < 5000; ++i) {
    EXPECT_EQ(8, Hbitwrite(bitid1, 8, (i & 0xFF)));
  }
  EXPECT_EQ(SUCCEED, Hendbitaccess(bitid1, 0));

  // Now open the existing element with Hstartbitwrite
  // This triggers exists == TRUE, max_offset > 0, pre-reading first block
  const int32_t bitid2 = Hstartbitwrite(fid, 800, 1, 0);
  ASSERT_NE(bitid2, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(bitid2));

  // Seek to byte 1000 and write > 4096 bytes to trigger pre-reading next block
  // during write
  EXPECT_EQ(SUCCEED, Hbitseek(bitid2, 1000, 0));
  for (int i = 0; i < 4100; ++i) {
    EXPECT_EQ(8, Hbitwrite(bitid2, 8, 0xAA));
  }
  EXPECT_EQ(SUCCEED, Hendbitaccess(bitid2, 0));
}

TEST_F(HbitioTest, EmptyElementBitRead) {
  const std::string path = GetTempFilePath("hbitio_empty.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  // Create empty element (0 bytes)
  const int32_t aid = Hstartwrite(fid, 900, 1, 0);
  ASSERT_NE(aid, FAIL);
  EXPECT_EQ(SUCCEED, Hendaccess(aid));

  // Open with Hstartbitread -> max_offset == 0 <= byte_offset (hits lines
  // 136-139)
  const int32_t bitid = Hstartbitread(fid, 900, 1);
  ASSERT_NE(bitid, FAIL);
  absl::Cleanup bit_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hendbitaccess(bitid, 0));
  };

  uint32_t val = 0;
  EXPECT_EQ(0, Hbitread(bitid, 8, &val));
}

TEST_F(HbitioTest, BitIdCachingInReadAndWrite) {
  const std::string path = GetTempFilePath("hbitio_cache.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  const int32_t id1 = Hstartbitwrite(fid, 1000, 1, 0);
  const int32_t id2 = Hstartbitwrite(fid, 1001, 1, 0);
  ASSERT_NE(id1, FAIL);
  ASSERT_NE(id2, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(id1));
  EXPECT_EQ(SUCCEED, Hbitappendable(id2));

  // Consecutive writes to id1 (cached)
  EXPECT_EQ(8, Hbitwrite(id1, 8, 0x11));
  EXPECT_EQ(8, Hbitwrite(id1, 8, 0x22));
  // Switch to id2 (non-cached)
  EXPECT_EQ(8, Hbitwrite(id2, 8, 0x33));
  // Switch back to id1
  EXPECT_EQ(8, Hbitwrite(id1, 8, 0x44));

  EXPECT_EQ(SUCCEED, Hendbitaccess(id1, 0));
  EXPECT_EQ(SUCCEED, Hendbitaccess(id2, 0));

  const int32_t rid1 = Hstartbitread(fid, 1000, 1);
  const int32_t rid2 = Hstartbitread(fid, 1001, 1);
  ASSERT_NE(rid1, FAIL);
  ASSERT_NE(rid2, FAIL);

  uint32_t val = 0;
  // Consecutive reads from rid1 (cached)
  EXPECT_EQ(8, Hbitread(rid1, 8, &val));
  EXPECT_EQ(val, 0x11U);
  EXPECT_EQ(8, Hbitread(rid1, 8, &val));
  EXPECT_EQ(val, 0x22U);

  // Switch to rid2 (non-cached)
  EXPECT_EQ(8, Hbitread(rid2, 8, &val));
  EXPECT_EQ(val, 0x33U);

  // Switch back to rid1
  EXPECT_EQ(8, Hbitread(rid1, 8, &val));
  EXPECT_EQ(val, 0x44U);

  EXPECT_EQ(SUCCEED, Hendbitaccess(rid1, 0));
  EXPECT_EQ(SUCCEED, Hendbitaccess(rid2, 0));
}

TEST_F(HbitioTest, ReadClampedCount) {
  const std::string path = GetTempFilePath("hbitio_clamp.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  const int32_t wid = Hstartbitwrite(fid, 1050, 1, 0);
  ASSERT_NE(wid, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(wid));
  EXPECT_EQ(32, Hbitwrite(wid, 32, 0x12345678));
  EXPECT_EQ(SUCCEED, Hendbitaccess(wid, 0));

  const int32_t rid = Hstartbitread(fid, 1050, 1);
  ASSERT_NE(rid, FAIL);
  absl::Cleanup read_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hendbitaccess(rid, 0));
  };

  // Read with count > 32 (clamped to DATANUM = 32, hits line 452)
  uint32_t val = 0;
  EXPECT_EQ(32, Hbitread(rid, 64, &val));
  EXPECT_EQ(val, 0x12345678U);
}

TEST_F(HbitioTest, PartialReadBufferRefill) {
  const std::string path = GetTempFilePath("hbitio_partial_refill.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  const int32_t wid = Hstartbitwrite(fid, 1060, 1, 0);
  ASSERT_NE(wid, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(wid));

  // Write 4096 bytes
  for (int i = 0; i < 4096; ++i) {
    EXPECT_EQ(8, Hbitwrite(wid, 8, 0x55));
  }
  // Write 1 more byte: 0b10100000
  EXPECT_EQ(8, Hbitwrite(wid, 8, 0xA0));
  EXPECT_EQ(SUCCEED, Hendbitaccess(wid, 0));

  const int32_t rid = Hstartbitread(fid, 1060, 1);
  ASSERT_NE(rid, FAIL);
  absl::Cleanup read_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hendbitaccess(rid, 0));
  };

  // Read first 4096 bytes so bytep == bytez
  for (int i = 0; i < 4096; ++i) {
    uint32_t val = 0;
    EXPECT_EQ(8, Hbitread(rid, 8, &val));
    ASSERT_EQ(val, 0x55U);
  }

  // Now bytep == bytez. Read partial byte (3 bits) -> hits lines 493-506!
  uint32_t partial_val = 0;
  EXPECT_EQ(3, Hbitread(rid, 3, &partial_val));
  // High 3 bits of 0xA0 (0b10100000) are 0b101 = 5
  EXPECT_EQ(partial_val, 5U);
}

TEST_F(HbitioTest, ExistingElementMultiByteWriteBufferRefill) {
  const std::string path = GetTempFilePath("hbitio_exist_multibyte.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    EXPECT_EQ(SUCCEED, Hclose(fid));
    std::filesystem::remove(path);
  };

  // Create an element with 8192 bytes
  const int32_t wid = Hstartbitwrite(fid, 1070, 1, 0);
  ASSERT_NE(wid, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(wid));
  for (int i = 0; i < 8192; ++i) {
    EXPECT_EQ(8, Hbitwrite(wid, 8, 0x00));
  }
  EXPECT_EQ(SUCCEED, Hendbitaccess(wid, 0));

  // Open existing element for write
  const int32_t wid2 = Hstartbitwrite(fid, 1070, 1, 0);
  ASSERT_NE(wid2, FAIL);
  EXPECT_EQ(SUCCEED, Hbitappendable(wid2));

  // Seek to byte 4094
  EXPECT_EQ(SUCCEED, Hbitseek(wid2, 4094, 0));
  // Multi-byte write spanning byte 4095 and 4096 -> hits lines 374-383 in while
  // loop!
  EXPECT_EQ(32, Hbitwrite(wid2, 32, 0x11223344));
  EXPECT_EQ(SUCCEED, Hendbitaccess(wid2, 0));
}

}  // namespace
}  // namespace hdf4
