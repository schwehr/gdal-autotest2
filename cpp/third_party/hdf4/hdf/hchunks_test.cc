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

// Tests chunked special data elements (hdf/src/hchunks.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/hchunks.c
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/test/chunks.c

#include <climits>
#include <cstdint>
#include <cstring>
#include <filesystem>  // NOLINT
#include <numeric>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"

#ifdef FAIL
#undef FAIL
#endif
#ifdef SUCCEED
#undef SUCCEED
#endif

#include "third_party/hdf4/hdf/src/hdf.h"
// clang-format off
#include "third_party/hdf4/hdf/src/hdf_priv.h"
#include "third_party/hdf4/hdf/src/hfile_priv.h"
#include "third_party/hdf4/hdf/src/hchunks_priv.h"
#include "third_party/hdf4/hdf/src/hcomp.h"
#include "third_party/hdf4/hdf/src/hproto.h"
#include "third_party/hdf4/hdf/src/mcache_priv.h"
#include "third_party/hdf4/hdf/src/tbbt_priv.h"
#include "third_party/hdf4/hdf/src/vg.h"
// clang-format on

namespace hdf4 {
namespace {

using ::testing::ElementsAre;
using ::testing::Eq;
using ::testing::Ge;
using ::testing::Gt;
using ::testing::Ne;

std::string GetTempFilePath(const std::string& filename) {
  return (std::filesystem::path(::testing::TempDir()) / filename).string();
}

TEST(HchunksTest, InvalidArgumentsHMCcreate) {
  const std::string path = GetTempFilePath("hchunks_inv_create.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 2,
      .nt_size = static_cast<int32_t>(sizeof(uint8_t)),
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill_val = 0;

  // Invalid file ID
  EXPECT_THAT(HMCcreate(FAIL, 1000, 1, 1, 1, &fill_val, &chk), Eq(FAIL));

  // NULL chunk array
  EXPECT_THAT(HMCcreate(fid, 1000, 1, 1, 1, &fill_val, nullptr), Eq(FAIL));

  // Special tag directly passed
  EXPECT_THAT(HMCcreate(fid, static_cast<uint16_t>(0x8000 | 1000), 1, 1, 1,
                        &fill_val, &chk),
              Eq(FAIL));

  // Valid create
  const int32_t aid = HMCcreate(fid, 1000, 1, 1, 1, &fill_val, &chk);
  ASSERT_THAT(aid, Ne(FAIL));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
}

TEST(HchunksTest, ReadOnlyFileHMCcreateFails) {
  const std::string path = GetTempFilePath("hchunks_ro_create.hdf");
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 2,
      .nt_size = 1,
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill_val = 0;
  EXPECT_THAT(HMCcreate(fid, 1000, 1, 1, 1, &fill_val, &chk), Eq(FAIL));
}

TEST(HchunksTest, Basic2DChunkedElementLifecycle) {
  const std::string path = GetTempFilePath("hchunks_2d_basic.hdf");
  constexpr uint16_t kTag = 1050;
  constexpr uint16_t kRef = 2;
  constexpr uint8_t kFillVal = 0xAA;

  DIM_DEF dims[2] = {
      {.dim_length = 4, .chunk_length = 2, .distrib_type = 0},
      {.dim_length = 4, .chunk_length = 2, .distrib_type = 0},
  };
  HCHUNK_DEF chk = {
      .chunk_size = 4,  // 2 x 2
      .nt_size = static_cast<int32_t>(sizeof(uint8_t)),
      .num_dims = 2,
      .pdims = dims,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };

  // 1. Create and write chunk (0, 0)
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    uint8_t fill = kFillVal;
    const int32_t aid = HMCcreate(fid, kTag, kRef, 1, 1, &fill, &chk);
    ASSERT_THAT(aid, Ne(FAIL));

    // Write chunk (0, 0) via HMCwriteChunk
    int32_t origin00[2] = {0, 0};
    const uint8_t chunk0[4] = {1, 2, 3, 4};
    EXPECT_THAT(HMCwriteChunk(aid, origin00, chunk0), Eq(4));

    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  }

  // 2. Reopen for write, write chunk (1, 1) and chunk (1, 0) via HMCwriteChunk
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_RDWR, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    const int32_t aid = Hstartwrite(fid, kTag, kRef, 16);
    ASSERT_THAT(aid, Ne(FAIL));

    // Write chunk (1, 1)
    int32_t origin11[2] = {1, 1};
    const uint8_t chunk11[4] = {13, 14, 15, 16};
    EXPECT_THAT(HMCwriteChunk(aid, origin11, chunk11), Eq(4));

    // Write chunk (1, 0)
    int32_t origin10[2] = {1, 0};
    const uint8_t chunk10[4] = {9, 10, 11, 12};
    EXPECT_THAT(HMCwriteChunk(aid, origin10, chunk10), Eq(4));

    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  }

  // 3. Reopen for read, verify Hinquire, HDget_special_info, HMCreadChunk,
  // Hread
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    const int32_t aid = Hstartread(fid, kTag, kRef);
    ASSERT_THAT(aid, Ne(FAIL));

    // Inquire about element
    int32_t file_id = 0;
    uint16_t inq_tag = 0;
    uint16_t inq_ref = 0;
    int32_t inq_len = 0;
    int32_t inq_off = 0;
    int32_t inq_posn = 0;
    int16_t inq_acc = 0;
    int16_t inq_special = 0;
    EXPECT_THAT(Hinquire(aid, &file_id, &inq_tag, &inq_ref, &inq_len, &inq_off,
                         &inq_posn, &inq_acc, &inq_special),
                Eq(SUCCEED));
    EXPECT_THAT(inq_special, Eq(SPECIAL_CHUNKED));
    EXPECT_THAT(inq_len, Eq(16));
    EXPECT_THAT(inq_ref, Eq(kRef));

    // Query special info
    sp_info_block_t sp_info = {};
    EXPECT_THAT(HDget_special_info(aid, &sp_info), Eq(SUCCEED));
    EXPECT_THAT(sp_info.ndims, Eq(2));
    ASSERT_THAT(sp_info.cdims, Ne(nullptr));
    EXPECT_THAT(sp_info.cdims[0], Eq(2));
    EXPECT_THAT(sp_info.cdims[1], Eq(2));
    std::free(sp_info.cdims);

    // Read chunk (0, 0)
    int32_t origin00[2] = {0, 0};
    uint8_t read_chunk[4] = {0};
    EXPECT_THAT(HMCreadChunk(aid, origin00, read_chunk), Eq(4));
    EXPECT_THAT(read_chunk[0], Eq(1));
    EXPECT_THAT(read_chunk[1], Eq(2));
    EXPECT_THAT(read_chunk[2], Eq(3));
    EXPECT_THAT(read_chunk[3], Eq(4));

    // Read unwritten chunk (0, 1) -> must contain fill values!
    int32_t origin01[2] = {0, 1};
    EXPECT_THAT(HMCreadChunk(aid, origin01, read_chunk), Eq(4));
    EXPECT_THAT(read_chunk[0], Eq(kFillVal));
    EXPECT_THAT(read_chunk[1], Eq(kFillVal));
    EXPECT_THAT(read_chunk[2], Eq(kFillVal));
    EXPECT_THAT(read_chunk[3], Eq(kFillVal));

    // Read chunk (1, 1)
    int32_t origin11[2] = {1, 1};
    EXPECT_THAT(HMCreadChunk(aid, origin11, read_chunk), Eq(4));
    EXPECT_THAT(read_chunk[0], Eq(13));
    EXPECT_THAT(read_chunk[1], Eq(14));
    EXPECT_THAT(read_chunk[2], Eq(15));
    EXPECT_THAT(read_chunk[3], Eq(16));

    // Read entire 16 bytes via sequential stream read
    EXPECT_THAT(Hseek(aid, 0, DF_START), Eq(SUCCEED));
    uint8_t full_read[16] = {0};
    EXPECT_THAT(Hread(aid, 16, full_read), Eq(16));
    EXPECT_THAT(full_read[0], Eq(1));
    EXPECT_THAT(full_read[1], Eq(2));
    EXPECT_THAT(full_read[2], Eq(kFillVal));
    EXPECT_THAT(full_read[3], Eq(kFillVal));
    EXPECT_THAT(full_read[4], Eq(3));
    EXPECT_THAT(full_read[5], Eq(4));
    EXPECT_THAT(full_read[6], Eq(kFillVal));
    EXPECT_THAT(full_read[7], Eq(kFillVal));
    EXPECT_THAT(full_read[8], Eq(9));
    EXPECT_THAT(full_read[9], Eq(10));
    EXPECT_THAT(full_read[10], Eq(13));
    EXPECT_THAT(full_read[11], Eq(14));
    EXPECT_THAT(full_read[12], Eq(11));
    EXPECT_THAT(full_read[13], Eq(12));
    EXPECT_THAT(full_read[14], Eq(15));
    EXPECT_THAT(full_read[15], Eq(16));

    // Test seek operations
    EXPECT_THAT(Hseek(aid, 8, DF_START), Eq(SUCCEED));
    uint8_t half[4] = {0};
    EXPECT_THAT(Hread(aid, 4, half), Eq(4));
    EXPECT_THAT(half[0], Eq(9));
    EXPECT_THAT(half[1], Eq(10));
    EXPECT_THAT(half[2], Eq(13));
    EXPECT_THAT(half[3], Eq(14));

    EXPECT_THAT(Hseek(aid, -4, DF_END), Eq(SUCCEED));
    EXPECT_THAT(Hread(aid, 4, half), Eq(4));
    EXPECT_THAT(half[0], Eq(11));
    EXPECT_THAT(half[1], Eq(12));
    EXPECT_THAT(half[2], Eq(15));
    EXPECT_THAT(half[3], Eq(16));

    // Verify HMCPgetnumrecs
    auto* arec = static_cast<accrec_t*>(HAatom_object(aid));
    ASSERT_THAT(arec, Ne(nullptr));
    int32_t num_recs = 0;
    EXPECT_THAT(HMCPgetnumrecs(arec, &num_recs), Eq(SUCCEED));
    EXPECT_THAT(num_recs, Eq(3));  // chunks (0,0), (1,1), (1,0) written

    // Test HMCsetMaxcache
    EXPECT_THAT(HMCsetMaxcache(aid, 10, 0), Eq(10));

    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

    // Query data size on uncompressed chunked element
    int32_t comp_size = -1;
    int32_t orig_size = -1;
    EXPECT_THAT(HCPgetdatasize(fid, kTag, kRef, &comp_size, &orig_size),
                Eq(SUCCEED));
    EXPECT_THAT(orig_size, Eq(12));  // 3 records * 4 bytes per chunk = 12
    EXPECT_THAT(comp_size, Eq(12));

    // Query data info on uncompressed chunk (exercises lines 2036-2042)
    int32_t coord[2] = {0, 0};
    int32_t offset = 0;
    int32_t len = 0;
    EXPECT_THAT(HMCgetdatainfo(fid, kTag, kRef, coord, 0, 1, &offset, &len),
                Eq(1));
    EXPECT_THAT(offset, Gt(0));
    EXPECT_THAT(len, Eq(4));
  }
}

TEST(HchunksTest, ChunkedElementWithDeflateCompression) {
  const std::string path = GetTempFilePath("hchunks_deflate.hdf");
  constexpr uint16_t kTag = 1060;
  constexpr uint16_t kRef = 5;

  DIM_DEF dims[2] = {
      {.dim_length = 6, .chunk_length = 3, .distrib_type = 0},
      {.dim_length = 6, .chunk_length = 3, .distrib_type = 0},
  };
  comp_info cinfo = {};
  cinfo.deflate_setting.level = 6;
  model_info minfo = {};
  HCHUNK_DEF chk = {
      .chunk_size = 9,
      .nt_size = static_cast<int32_t>(sizeof(int32_t)),
      .num_dims = 2,
      .pdims = dims,
      .chunk_flag = SPECIAL_COMP,
      .comp_type = COMP_CODE_DEFLATE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = &cinfo,
      .minfo = &minfo,
  };

  int32_t fill_val = -1;
  std::vector<int32_t> chunk_data(9, 42);

  // 1. Create and write chunk (0, 0)
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    const int32_t aid =
        HMCcreate(fid, kTag, kRef, 1, sizeof(int32_t), &fill_val, &chk);
    ASSERT_THAT(aid, Ne(FAIL));

    // Verify HMCgetcompress on writer
    auto* arec = static_cast<accrec_t*>(HAatom_object(aid));
    ASSERT_THAT(arec, Ne(nullptr));
    comp_coder_t comp_type = COMP_CODE_INVALID;
    comp_info retrieved_info = {};
    EXPECT_THAT(HMCgetcompress(arec, &comp_type, &retrieved_info), Eq(SUCCEED));
    EXPECT_THAT(comp_type, Eq(COMP_CODE_DEFLATE));

    int32_t origin00[2] = {0, 0};
    EXPECT_THAT(HMCwriteChunk(aid, origin00, chunk_data.data()), Eq(36));

    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  }

  // 2. Reopen and verify compression type and data readback
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    // Test compression query via HCPgetcomptype
    comp_coder_t dd_comp_type = COMP_CODE_INVALID;
    EXPECT_THAT(HCPgetcomptype(fid, kTag, kRef, &dd_comp_type), Eq(SUCCEED));
    EXPECT_THAT(dd_comp_type, Eq(COMP_CODE_DEFLATE));

    // Open chunked element for read
    const int32_t aid = Hstartread(fid, kTag, kRef);
    ASSERT_THAT(aid, Ne(FAIL));

    auto* arec = static_cast<accrec_t*>(HAatom_object(aid));
    ASSERT_THAT(arec, Ne(nullptr));
    comp_coder_t read_comp_type = COMP_CODE_INVALID;
    comp_info read_info = {};
    EXPECT_THAT(HMCgetcompress(arec, &read_comp_type, &read_info), Eq(SUCCEED));
    EXPECT_THAT(read_comp_type, Eq(COMP_CODE_DEFLATE));

    // Read chunk back
    int32_t origin00[2] = {0, 0};
    std::vector<int32_t> read_data(9, 0);
    EXPECT_THAT(HMCreadChunk(aid, origin00, read_data.data()), Eq(36));
    EXPECT_THAT(read_data, ElementsAre(42, 42, 42, 42, 42, 42, 42, 42, 42));

    // Query special info on compressed chunked element
    sp_info_block_t sp_info = {};
    EXPECT_THAT(HDget_special_info(aid, &sp_info), Eq(SUCCEED));
    EXPECT_THAT(sp_info.comp_type, Eq(COMP_CODE_DEFLATE));
    EXPECT_THAT(sp_info.model_type, Eq(COMP_MODEL_STDIO));
    std::free(sp_info.cdims);

    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

    // Query data size on compressed chunked element (exercises lines 2255-2365)
    int32_t comp_size = -1;
    int32_t orig_size = -1;
    EXPECT_THAT(HCPgetdatasize(fid, kTag, kRef, &comp_size, &orig_size),
                Eq(SUCCEED));
    EXPECT_THAT(orig_size, Eq(36));  // 1 record * 36 bytes = 36
    EXPECT_THAT(comp_size, Gt(0));

    EXPECT_THAT(HCPgetdatasize(fid, kTag, kRef, nullptr, &orig_size),
                Eq(SUCCEED));
    EXPECT_THAT(HCPgetdatasize(fid, kTag, kRef, &comp_size, nullptr),
                Eq(SUCCEED));
    EXPECT_THAT(HCPgetdatasize(fid, kTag, kRef, nullptr, nullptr), Eq(SUCCEED));

    // Query HMCgetdatainfo on chunk
    int32_t coord[2] = {0, 0};
    int32_t offset = 0;
    int32_t len = 0;
    EXPECT_THAT(HMCgetdatainfo(fid, kTag, kRef, coord, 0, 1, &offset, &len),
                Eq(1));
    EXPECT_THAT(offset, Gt(0));
    EXPECT_THAT(len, Gt(0));

    // Query HMCgetdatainfo on unwritten chunk
    int32_t unwritten_coord[2] = {1, 1};
    int32_t unwritten_off = -1;
    int32_t unwritten_len = -1;
    EXPECT_THAT(HMCgetdatainfo(fid, kTag, kRef, unwritten_coord, 0, 1,
                               &unwritten_off, &unwritten_len),
                Eq(0));
    EXPECT_THAT(unwritten_off, Eq(0));
    EXPECT_THAT(unwritten_len, Eq(0));
  }
}

TEST(HchunksTest, ChunkedElementWithRleCompression) {
  const std::string path = GetTempFilePath("hchunks_rle.hdf");
  constexpr uint16_t kTag = 1070;
  constexpr uint16_t kRef = 7;

  DIM_DEF dim = {.dim_length = 16, .chunk_length = 8, .distrib_type = 0};
  comp_info cinfo = {};
  model_info minfo = {};
  HCHUNK_DEF chk = {
      .chunk_size = 8 * static_cast<int32_t>(sizeof(uint8_t)),
      .nt_size = static_cast<int32_t>(sizeof(uint8_t)),
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = SPECIAL_COMP,
      .comp_type = COMP_CODE_RLE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = &cinfo,
      .minfo = &minfo,
  };

  uint8_t fill_val = 0;
  std::vector<uint8_t> chunk_data = {5, 5, 5, 5, 5, 5, 5, 5};

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    const int32_t aid =
        HMCcreate(fid, kTag, kRef, 1, sizeof(uint8_t), &fill_val, &chk);
    ASSERT_THAT(aid, Ne(FAIL));

    int32_t origin0[1] = {0};
    EXPECT_THAT(HMCwriteChunk(aid, origin0, chunk_data.data()), Eq(8));

    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  }

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    const int32_t aid = Hstartread(fid, kTag, kRef);
    ASSERT_THAT(aid, Ne(FAIL));

    int32_t origin0[1] = {0};
    std::vector<uint8_t> read_data(8, 0);
    EXPECT_THAT(HMCreadChunk(aid, origin0, read_data.data()), Eq(8));
    EXPECT_THAT(read_data, ElementsAre(5, 5, 5, 5, 5, 5, 5, 5));

    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  }
}

TEST(HchunksTest, MultiDimensionalAndOddDimensions) {
  const std::string path = GetTempFilePath("hchunks_multidim.hdf");
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    // 1-D array: length 15, chunk length 4 (tests odd size: 15 % 4 = 3)
    {
      DIM_DEF dim = {.dim_length = 15, .chunk_length = 4, .distrib_type = 0};
      HCHUNK_DEF chk = {
          .chunk_size = 4,
          .nt_size = 1,
          .num_dims = 1,
          .pdims = &dim,
          .chunk_flag = 0,
          .comp_type = COMP_CODE_NONE,
          .model_type = COMP_MODEL_STDIO,
          .cinfo = nullptr,
          .minfo = nullptr,
      };
      uint8_t fill = 0;
      const int32_t aid = HMCcreate(fid, 1080, 1, 1, 1, &fill, &chk);
      ASSERT_THAT(aid, Ne(FAIL));

      uint8_t wdata[8] = {1, 2, 3, 4, 5, 6, 7, 8};
      EXPECT_THAT(Hwrite(aid, 8, wdata), Eq(8));
      EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
    }

    // 3-D array: 4x4x4 (64 bytes), chunk size 2x2x2 (8 bytes)
    {
      DIM_DEF dims3[3] = {
          {.dim_length = 4, .chunk_length = 2, .distrib_type = 0},
          {.dim_length = 4, .chunk_length = 2, .distrib_type = 0},
          {.dim_length = 4, .chunk_length = 2, .distrib_type = 0},
      };
      HCHUNK_DEF chk3 = {
          .chunk_size = 8,
          .nt_size = 1,
          .num_dims = 3,
          .pdims = dims3,
          .chunk_flag = 0,
          .comp_type = COMP_CODE_NONE,
          .model_type = COMP_MODEL_STDIO,
          .cinfo = nullptr,
          .minfo = nullptr,
      };
      uint8_t fill = 0xFF;
      const int32_t aid = HMCcreate(fid, 1080, 2, 1, 1, &fill, &chk3);
      ASSERT_THAT(aid, Ne(FAIL));

      int32_t origin[3] = {1, 1, 1};
      uint8_t cdata[8] = {1, 2, 3, 4, 5, 6, 7, 8};
      EXPECT_THAT(HMCwriteChunk(aid, origin, cdata), Eq(8));

      uint8_t rdata[8] = {0};
      EXPECT_THAT(HMCreadChunk(aid, origin, rdata), Eq(8));
      EXPECT_THAT(rdata[0], Eq(1));
      EXPECT_THAT(rdata[7], Eq(8));

      EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
    }

    // 4-D array: 2x2x2x2 (16 bytes), chunk size 2x1x2x1 (4 bytes)
    {
      DIM_DEF dims4[4] = {
          {.dim_length = 2, .chunk_length = 2, .distrib_type = 0},
          {.dim_length = 2, .chunk_length = 1, .distrib_type = 0},
          {.dim_length = 2, .chunk_length = 2, .distrib_type = 0},
          {.dim_length = 2, .chunk_length = 1, .distrib_type = 0},
      };
      HCHUNK_DEF chk4 = {
          .chunk_size = 4,
          .nt_size = 1,
          .num_dims = 4,
          .pdims = dims4,
          .chunk_flag = 0,
          .comp_type = COMP_CODE_NONE,
          .model_type = COMP_MODEL_STDIO,
          .cinfo = nullptr,
          .minfo = nullptr,
      };
      uint8_t fill = 0;
      const int32_t aid = HMCcreate(fid, 1080, 3, 1, 1, &fill, &chk4);
      ASSERT_THAT(aid, Ne(FAIL));

      int32_t origin[4] = {0, 1, 0, 1};
      uint8_t cdata[4] = {10, 20, 30, 40};
      EXPECT_THAT(HMCwriteChunk(aid, origin, cdata), Eq(4));

      uint8_t rdata[4] = {0};
      EXPECT_THAT(HMCreadChunk(aid, origin, rdata), Eq(4));
      EXPECT_THAT(rdata[0], Eq(10));
      EXPECT_THAT(rdata[3], Eq(40));

      EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
    }
  }

  // Reopen read-only: exercises HMCIstaccess decoding odd dims and multidim
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    // Read back odd dimension element
    const int32_t aid1 = Hstartread(fid, 1080, 1);
    ASSERT_THAT(aid1, Ne(FAIL));
    uint8_t rdata1[8] = {0};
    EXPECT_THAT(Hread(aid1, 8, rdata1), Eq(8));
    EXPECT_THAT(rdata1[0], Eq(1));
    EXPECT_THAT(rdata1[7], Eq(8));
    EXPECT_THAT(Hendaccess(aid1), Eq(SUCCEED));

    // Read back 3-D element
    const int32_t aid2 = Hstartread(fid, 1080, 2);
    ASSERT_THAT(aid2, Ne(FAIL));
    uint8_t rdata2[8] = {0};
    int32_t origin3[3] = {1, 1, 1};
    EXPECT_THAT(HMCreadChunk(aid2, origin3, rdata2), Eq(8));
    EXPECT_THAT(rdata2[0], Eq(1));
    EXPECT_THAT(rdata2[7], Eq(8));
    EXPECT_THAT(Hendaccess(aid2), Eq(SUCCEED));

    // Read back 4-D element
    const int32_t aid3 = Hstartread(fid, 1080, 3);
    ASSERT_THAT(aid3, Ne(FAIL));
    uint8_t rdata3[4] = {0};
    int32_t origin4[4] = {0, 1, 0, 1};
    EXPECT_THAT(HMCreadChunk(aid3, origin4, rdata3), Eq(4));
    EXPECT_THAT(rdata3[0], Eq(10));
    EXPECT_THAT(rdata3[3], Eq(40));
    EXPECT_THAT(Hendaccess(aid3), Eq(SUCCEED));
  }
}

TEST(HchunksTest, CachePagingAndEviction) {
  const std::string path = GetTempFilePath("hchunks_cache_paging.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // 1-D array of 8 chunks (chunk size 4)
  DIM_DEF dim = {.dim_length = 32, .chunk_length = 4, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 4,
      .nt_size = 1,
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill = 0;
  const int32_t aid = HMCcreate(fid, 1090, 1, 1, 1, &fill, &chk);
  ASSERT_THAT(aid, Ne(FAIL));

  // Set maximum cache size to 2 (forces evictions as we write 8 chunks)
  EXPECT_THAT(HMCsetMaxcache(aid, 2, 0), Eq(2));

  for (int32_t c = 0; c < 8; ++c) {
    int32_t origin[1] = {c};
    uint8_t cdata[4] = {
        static_cast<uint8_t>(c * 10 + 1), static_cast<uint8_t>(c * 10 + 2),
        static_cast<uint8_t>(c * 10 + 3), static_cast<uint8_t>(c * 10 + 4)};
    EXPECT_THAT(HMCwriteChunk(aid, origin, cdata), Eq(4));
  }

  // Read back in reverse order (forces page-ins)
  for (int32_t c = 7; c >= 0; --c) {
    int32_t origin[1] = {c};
    uint8_t rdata[4] = {0};
    EXPECT_THAT(HMCreadChunk(aid, origin, rdata), Eq(4));
    EXPECT_THAT(rdata[0], Eq(c * 10 + 1));
    EXPECT_THAT(rdata[3], Eq(c * 10 + 4));
  }

  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
}

TEST(HchunksTest, ErrorHandlingAndEdgeCases) {
  const std::string path = GetTempFilePath("hchunks_errors.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 2,
      .nt_size = 1,
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill = 0;
  const int32_t aid = HMCcreate(fid, 1100, 1, 1, 1, &fill, &chk);
  ASSERT_THAT(aid, Ne(FAIL));

  int32_t origin[1] = {0};
  uint8_t data[2] = {1, 2};

  // HMCwriteChunk invalid args
  EXPECT_THAT(HMCwriteChunk(FAIL, origin, data), Eq(FAIL));
  EXPECT_THAT(HMCwriteChunk(aid, nullptr, data), Eq(FAIL));
  EXPECT_THAT(HMCwriteChunk(aid, origin, nullptr), Eq(FAIL));

  // HMCreadChunk invalid args
  EXPECT_THAT(HMCreadChunk(FAIL, origin, data), Eq(FAIL));
  EXPECT_THAT(HMCreadChunk(aid, nullptr, data), Eq(FAIL));
  EXPECT_THAT(HMCreadChunk(aid, origin, nullptr), Eq(FAIL));

  // HMCsetMaxcache invalid args
  EXPECT_THAT(HMCsetMaxcache(FAIL, 5, 0), Eq(FAIL));
  EXPECT_THAT(HMCsetMaxcache(aid, 0, 0), Eq(FAIL));  // maxcache < 1 fails

  // HMCgetcompress on NULL special_info
  accrec_t dummy_arec = {};
  dummy_arec.special_info = nullptr;
  comp_coder_t coder = COMP_CODE_INVALID;
  comp_info retrieved_info = {};
  EXPECT_THAT(HMCgetcompress(&dummy_arec, &coder, &retrieved_info), Eq(FAIL));

  // HMCPcloseAID on NULL special_info
  EXPECT_THAT(HMCPcloseAID(&dummy_arec), Eq(FAIL));

  // HMCPgetnumrecs on NULL
  int32_t num_recs = 0;
  EXPECT_THAT(HMCPgetnumrecs(nullptr, &num_recs), Eq(FAIL));
  auto* arec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_THAT(arec, Ne(nullptr));
  EXPECT_THAT(HMCPgetnumrecs(arec, nullptr), Eq(FAIL));

  // HMCgetdatainfo invalid args
  int32_t off = 0;
  int32_t len = 0;
  EXPECT_THAT(HMCgetdatainfo(fid, 1100, 1, origin, 0, 0, &off, &len), Eq(FAIL));
  EXPECT_THAT(HMCgetdatainfo(FAIL, 1100, 1, origin, 0, 1, &off, &len),
              Eq(FAIL));

  // Read with negative length
  EXPECT_THAT(Hread(aid, -5, data), Eq(FAIL));

  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
}

TEST(HchunksTest, StreamReadWriteAcrossChunkBoundaries) {
  const std::string path = GetTempFilePath("hchunks_stream_boundary.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  DIM_DEF dims[2] = {
      {.dim_length = 4, .chunk_length = 2, .distrib_type = 0},
      {.dim_length = 4, .chunk_length = 2, .distrib_type = 0},
  };
  HCHUNK_DEF chk = {
      .chunk_size = 4,
      .nt_size = 1,
      .num_dims = 2,
      .pdims = dims,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill = 0;
  const int32_t aid = HMCcreate(fid, 1200, 1, 1, 1, &fill, &chk);
  ASSERT_THAT(aid, Ne(FAIL));

  // Write 16 sequential bytes straddling multiple chunks
  uint8_t wdata[16];
  std::iota(wdata, wdata + 16, 1);
  EXPECT_THAT(Hwrite(aid, 16, wdata), Eq(16));

  // Seek and read across boundaries
  EXPECT_THAT(Hseek(aid, 0, DF_START), Eq(SUCCEED));
  uint8_t rdata[5] = {0};
  EXPECT_THAT(Hread(aid, 5, rdata), Eq(5));
  EXPECT_THAT(rdata[0], Eq(1));
  EXPECT_THAT(rdata[4], Eq(5));

  // Relative seek forward
  EXPECT_THAT(Hseek(aid, 3, DF_CURRENT), Eq(SUCCEED));
  EXPECT_THAT(Hread(aid, 3, rdata), Eq(3));
  EXPECT_THAT(rdata[0], Eq(9));
  EXPECT_THAT(rdata[2], Eq(11));

  // Relative seek backward
  EXPECT_THAT(Hseek(aid, -4, DF_CURRENT), Eq(SUCCEED));
  EXPECT_THAT(Hread(aid, 2, rdata), Eq(2));
  EXPECT_THAT(rdata[0], Eq(8));
  EXPECT_THAT(rdata[1], Eq(9));

  // Seek from end
  EXPECT_THAT(Hseek(aid, -2, DF_END), Eq(SUCCEED));
  EXPECT_THAT(Hread(aid, 2, rdata), Eq(2));
  EXPECT_THAT(rdata[0], Eq(15));
  EXPECT_THAT(rdata[1], Eq(16));

  // Invalid seek
  EXPECT_THAT(Hseek(aid, -1, DF_START), Eq(FAIL));
  EXPECT_THAT(Hseek(aid, 0, 999), Eq(FAIL));

  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
}

TEST(HchunksTest, MultipleConcurrentReaders) {
  const std::string path = GetTempFilePath("hchunks_concurrent.hdf");
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    DIM_DEF dim = {.dim_length = 8, .chunk_length = 4, .distrib_type = 0};
    HCHUNK_DEF chk = {
        .chunk_size = 4,
        .nt_size = 1,
        .num_dims = 1,
        .pdims = &dim,
        .chunk_flag = 0,
        .comp_type = COMP_CODE_NONE,
        .model_type = COMP_MODEL_STDIO,
        .cinfo = nullptr,
        .minfo = nullptr,
    };
    uint8_t fill = 0;
    const int32_t aid = HMCcreate(fid, 1210, 1, 1, 1, &fill, &chk);
    ASSERT_THAT(aid, Ne(FAIL));

    uint8_t wdata[8] = {10, 20, 30, 40, 50, 60, 70, 80};
    EXPECT_THAT(Hwrite(aid, 8, wdata), Eq(8));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  }

  // Open twice simultaneously (exercises lines 914-921)
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    const int32_t aid1 = Hstartread(fid, 1210, 1);
    ASSERT_THAT(aid1, Ne(FAIL));

    const int32_t aid2 = Hstartread(fid, 1210, 1);
    ASSERT_THAT(aid2, Ne(FAIL));

    uint8_t buf1[4] = {0};
    uint8_t buf2[4] = {0};
    EXPECT_THAT(Hread(aid1, 4, buf1), Eq(4));
    EXPECT_THAT(buf1[0], Eq(10));

    EXPECT_THAT(Hread(aid2, 4, buf2), Eq(4));
    EXPECT_THAT(buf2[0], Eq(10));

    EXPECT_THAT(Hendaccess(aid1), Eq(SUCCEED));
    EXPECT_THAT(Hendaccess(aid2), Eq(SUCCEED));
  }
}

TEST(HchunksTest, VariablePageCounts) {
  const std::string path = GetTempFilePath("hchunks_pages.hdf");
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    // npages = 32 (exercises lines 1565-1568: npages > 16 && npages < 128)
    {
      DIM_DEF dim = {.dim_length = 32, .chunk_length = 1, .distrib_type = 0};
      HCHUNK_DEF chk = {
          .chunk_size = 1,
          .nt_size = 1,
          .num_dims = 1,
          .pdims = &dim,
          .chunk_flag = 0,
          .comp_type = COMP_CODE_NONE,
          .model_type = COMP_MODEL_STDIO,
          .cinfo = nullptr,
          .minfo = nullptr,
      };
      uint8_t fill = 0;
      const int32_t aid = HMCcreate(fid, 1230, 1, 1, 1, &fill, &chk);
      ASSERT_THAT(aid, Ne(FAIL));
      uint8_t d = 99;
      EXPECT_THAT(Hwrite(aid, 1, &d), Eq(1));
      EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
    }

    // npages = 150 (exercises lines 1573-1577: npages >= 128)
    {
      DIM_DEF dim = {.dim_length = 150, .chunk_length = 1, .distrib_type = 0};
      HCHUNK_DEF chk = {
          .chunk_size = 1,
          .nt_size = 1,
          .num_dims = 1,
          .pdims = &dim,
          .chunk_flag = 0,
          .comp_type = COMP_CODE_NONE,
          .model_type = COMP_MODEL_STDIO,
          .cinfo = nullptr,
          .minfo = nullptr,
      };
      uint8_t fill = 0;
      const int32_t aid = HMCcreate(fid, 1230, 2, 1, 1, &fill, &chk);
      ASSERT_THAT(aid, Ne(FAIL));
      uint8_t d = 88;
      EXPECT_THAT(Hwrite(aid, 1, &d), Eq(1));
      EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
    }
  }

  // Reopen read-only to exercise reading elements with 32 and 150 pages
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    const int32_t aid1 = Hstartread(fid, 1230, 1);
    ASSERT_THAT(aid1, Ne(FAIL));
    uint8_t d1 = 0;
    EXPECT_THAT(Hread(aid1, 1, &d1), Eq(1));
    EXPECT_THAT(d1, Eq(99));
    EXPECT_THAT(Hendaccess(aid1), Eq(SUCCEED));

    const int32_t aid2 = Hstartread(fid, 1230, 2);
    ASSERT_THAT(aid2, Ne(FAIL));
    uint8_t d2 = 0;
    EXPECT_THAT(Hread(aid2, 1, &d2), Eq(1));
    EXPECT_THAT(d2, Eq(88));
    EXPECT_THAT(Hendaccess(aid2), Eq(SUCCEED));
  }
}

TEST(HchunksTest, ReadOnlyChunkedStartWriteFails) {
  const std::string path = GetTempFilePath("hchunks_ro_write.hdf");
  constexpr uint16_t kTag = 1240;
  constexpr uint16_t kRef = 1;
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));

    DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
    HCHUNK_DEF chk = {
        .chunk_size = 2,
        .nt_size = 1,
        .num_dims = 1,
        .pdims = &dim,
        .chunk_flag = 0,
        .comp_type = COMP_CODE_NONE,
        .model_type = COMP_MODEL_STDIO,
        .cinfo = nullptr,
        .minfo = nullptr,
    };
    uint8_t fill = 0;
    const int32_t aid = HMCcreate(fid, kTag, kRef, 1, 1, &fill, &chk);
    ASSERT_THAT(aid, Ne(FAIL));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  // Open read-only, attempting Hstartwrite should fail in HMCIstaccess (line
  // 855)
  const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  EXPECT_THAT(Hstartwrite(fid, kTag, kRef, 4), Eq(FAIL));
  EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
}

TEST(HchunksTest, OverwritingExistingChunk) {
  const std::string path = GetTempFilePath("hchunks_overwrite.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 2,
      .nt_size = 1,
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill = 0;
  const int32_t aid = HMCcreate(fid, 1250, 1, 1, 1, &fill, &chk);
  ASSERT_THAT(aid, Ne(FAIL));

  int32_t origin0[1] = {0};
  uint8_t cdata1[2] = {10, 20};
  EXPECT_THAT(HMCwriteChunk(aid, origin0, cdata1), Eq(2));

  // Overwrite chunk 0 with new data (exercises lines 2999-3003 in
  // HMCPchunkwrite)
  uint8_t cdata2[2] = {30, 40};
  EXPECT_THAT(HMCwriteChunk(aid, origin0, cdata2), Eq(2));

  uint8_t rdata[2] = {0};
  EXPECT_THAT(HMCreadChunk(aid, origin0, rdata), Eq(2));
  EXPECT_THAT(rdata[0], Eq(30));
  EXPECT_THAT(rdata[1], Eq(40));

  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
}

TEST(HchunksTest, ReadPastEofTruncatesAndWritePastEofFails) {
  const std::string path = GetTempFilePath("hchunks_eof.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 2,
      .nt_size = 1,
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill = 0;
  const int32_t aid = HMCcreate(fid, 1260, 1, 1, 1, &fill, &chk);
  ASSERT_THAT(aid, Ne(FAIL));

  uint8_t wdata[4] = {1, 2, 3, 4};
  EXPECT_THAT(Hwrite(aid, 4, wdata), Eq(4));

  // Writing length <= 0 should fail with DFE_RANGE (line 3248)
  uint8_t extra[2] = {99, 99};
  EXPECT_THAT(Hwrite(aid, 0, extra), Eq(FAIL));
  EXPECT_THAT(Hwrite(aid, -1, extra), Eq(FAIL));

  // Seek near end and read past EOF: should truncate to remaining bytes (lines
  // 2814, 2819)
  EXPECT_THAT(Hseek(aid, 3, DF_START), Eq(SUCCEED));
  uint8_t rdata[10] = {0};
  EXPECT_THAT(Hread(aid, 10, rdata), Eq(1));
  EXPECT_THAT(rdata[0], Eq(4));

  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
}

TEST(HchunksTest, UnlimitedDimensionChunkedElement) {
  const std::string path = GetTempFilePath("hchunks_unlimited.hdf");
  constexpr uint16_t kTag = 1270;
  constexpr uint16_t kRef = 1;
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    // dim_length == 0 marks dimension as UNLIMITED (lines 1529-1535)
    DIM_DEF dim = {.dim_length = 0, .chunk_length = 4, .distrib_type = 0};
    HCHUNK_DEF chk = {
        .chunk_size = 4,
        .nt_size = 1,
        .num_dims = 1,
        .pdims = &dim,
        .chunk_flag = 0,
        .comp_type = COMP_CODE_NONE,
        .model_type = COMP_MODEL_STDIO,
        .cinfo = nullptr,
        .minfo = nullptr,
    };
    uint8_t fill = 0x55;
    const int32_t aid = HMCcreate(fid, kTag, kRef, 1, 1, &fill, &chk);
    ASSERT_THAT(aid, Ne(FAIL));

    uint8_t wdata[4] = {11, 22, 33, 44};
    EXPECT_THAT(Hwrite(aid, 4, wdata), Eq(4));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  }

  {
    const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    const int32_t aid = Hstartread(fid, kTag, kRef);
    ASSERT_THAT(aid, Ne(FAIL));

    uint8_t rdata[4] = {0};
    EXPECT_THAT(Hread(aid, 4, rdata), Eq(4));
    EXPECT_THAT(rdata[0], Eq(11));
    EXPECT_THAT(rdata[3], Eq(44));

    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
  }
}

TEST(HchunksTest, DuplicateSpecialTagHMCcreateFails) {
  const std::string path = GetTempFilePath("hchunks_dup.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 2,
      .nt_size = 1,
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill = 0;
  const int32_t aid = HMCcreate(fid, 1280, 1, 1, 1, &fill, &chk);
  ASSERT_THAT(aid, Ne(FAIL));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  // Calling HMCcreate again on the same tag/ref fails with DFE_CANTMOD (lines
  // 1381-1385)
  EXPECT_THAT(HMCcreate(fid, 1280, 1, 1, 1, &fill, &chk), Eq(FAIL));
}

TEST(HchunksTest, UncompressedQueryFunctions) {
  const std::string path = GetTempFilePath("hchunks_uncomp_queries.hdf");
  constexpr uint16_t kTag = 1290;
  constexpr uint16_t kRef = 1;
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

    DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
    HCHUNK_DEF chk = {
        .chunk_size = 2,
        .nt_size = 1,
        .num_dims = 1,
        .pdims = &dim,
        .chunk_flag = 0,
        .comp_type = COMP_CODE_NONE,
        .model_type = COMP_MODEL_STDIO,
        .cinfo = nullptr,
        .minfo = nullptr,
    };
    uint8_t fill = 0;
    const int32_t aid = HMCcreate(fid, kTag, kRef, 1, 1, &fill, &chk);
    ASSERT_THAT(aid, Ne(FAIL));

    // HMCgetcompress on uncompressed element sets COMP_CODE_NONE (line 1814)
    auto* arec = static_cast<accrec_t*>(HAatom_object(aid));
    ASSERT_THAT(arec, Ne(nullptr));
    comp_coder_t ctype = COMP_CODE_INVALID;
    comp_info cinfo = {};
    EXPECT_THAT(HMCgetcompress(arec, &ctype, &cinfo), Eq(SUCCEED));
    EXPECT_THAT(ctype, Eq(COMP_CODE_NONE));

    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

    // HCPgetdatasize on element with no written chunks (num_recs == 0)
    int32_t comp_size = -1;
    int32_t orig_size = -1;
    EXPECT_THAT(HCPgetdatasize(fid, kTag, kRef, &comp_size, &orig_size),
                Eq(SUCCEED));
    EXPECT_THAT(comp_size, Eq(0));
    EXPECT_THAT(orig_size, Eq(0));

    // HCPgetcomptype on uncompressed chunked element sets COMP_CODE_NONE (line
    // 1923)
    comp_coder_t dd_ctype = COMP_CODE_INVALID;
    EXPECT_THAT(HCPgetcomptype(fid, kTag, kRef, &dd_ctype), Eq(SUCCEED));
    EXPECT_THAT(dd_ctype, Eq(COMP_CODE_NONE));
  }
}

TEST(HchunksTest, HMCsetMaxcacheAndSeekEdgeCases) {
  const std::string path = GetTempFilePath("hchunks_cache_seek.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 2,
      .nt_size = 1,
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill = 0;
  const int32_t aid = HMCcreate(fid, 1310, 1, 1, 1, &fill, &chk);
  ASSERT_THAT(aid, Ne(FAIL));

  // HMCsetMaxcache with invalid maxcache (< 0) -> FAIL (line 2446)
  EXPECT_THAT(HMCsetMaxcache(aid, -1, 0), Eq(FAIL));

  // HMCsetMaxcache on non-chunked AID -> FAIL (line 2449)
  EXPECT_THAT(HMCsetMaxcache(FAIL, 5, 0), Eq(FAIL));

  // Seek out of range (offset < 0) -> FAIL (line 2539)
  EXPECT_THAT(Hseek(aid, -50, DF_START), Eq(FAIL));

  // Seek relative resulting in negative offset -> FAIL
  EXPECT_THAT(Hseek(aid, -10, DF_CURRENT), Eq(FAIL));

  // Seek from DF_END beyond backwards -> FAIL
  EXPECT_THAT(Hseek(aid, -100, DF_END), Eq(FAIL));

  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
}

TEST(HchunksTest, InvalidArgumentsAndNonChunkedAccess) {
  const std::string path = GetTempFilePath("hchunks_invarr.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 2,
      .nt_size = 1,
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill = 0;
  const int32_t aid = HMCcreate(fid, 1320, 1, 1, 1, &fill, &chk);
  ASSERT_THAT(aid, Ne(FAIL));

  int32_t origin[1] = {0};
  uint8_t buf[2] = {1, 2};

  // HMCreadChunk invalid args (lines 2694, 2697)
  EXPECT_THAT(HMCreadChunk(FAIL, origin, buf), Eq(FAIL));
  EXPECT_THAT(HMCreadChunk(aid, nullptr, buf), Eq(FAIL));
  EXPECT_THAT(HMCreadChunk(aid, origin, nullptr), Eq(FAIL));

  // HMCwriteChunk invalid args (lines 3068, 3071)
  EXPECT_THAT(HMCwriteChunk(FAIL, origin, buf), Eq(FAIL));
  EXPECT_THAT(HMCwriteChunk(aid, nullptr, buf), Eq(FAIL));
  EXPECT_THAT(HMCwriteChunk(aid, origin, nullptr), Eq(FAIL));

  // funclist_t endaccess/inquire/info NULL access_rec checks
  EXPECT_THAT(chunked_funcs.endaccess(nullptr), Eq(FAIL));
  EXPECT_THAT(chunked_funcs.inquire(nullptr, nullptr, nullptr, nullptr, nullptr,
                                    nullptr, nullptr, nullptr, nullptr),
              Eq(FAIL));
  sp_info_block_t dummy_info = {};
  EXPECT_THAT(chunked_funcs.info(nullptr, &dummy_info), Eq(FAIL));
  EXPECT_THAT(chunked_funcs.seek(nullptr, 0, DF_START), Eq(FAIL));

  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  // Create a regular (non-chunked) element
  const int32_t reg_aid = Hstartwrite(fid, 1330, 1, 4);
  ASSERT_THAT(reg_aid, Ne(FAIL));
  uint8_t reg_data[4] = {1, 2, 3, 4};
  EXPECT_THAT(Hwrite(reg_aid, 4, reg_data), Eq(4));

  // Calling HMCreadChunk and HMCwriteChunk on non-chunked AID fails (lines
  // 2749, 3173)
  EXPECT_THAT(HMCreadChunk(reg_aid, origin, buf), Eq(FAIL));
  EXPECT_THAT(HMCwriteChunk(reg_aid, origin, buf), Eq(FAIL));

  // Calling HMCsetMaxcache on non-chunked AID fails
  EXPECT_THAT(HMCsetMaxcache(reg_aid, 5, 0), Eq(FAIL));

  EXPECT_THAT(Hendaccess(reg_aid), Eq(SUCCEED));
}

TEST(HchunksTest, HMCgetdatainfoEdgeCases) {
  const std::string path = GetTempFilePath("hchunks_datainfo_err.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  int32_t coord[1] = {0};
  int32_t off = 0;
  int32_t len = 0;

  // Invalid file ID (line 1992)
  EXPECT_THAT(HMCgetdatainfo(FAIL, 1000, 1, coord, 0, 1, &off, &len), Eq(FAIL));

  // Non-existent tag/ref (line 1995)
  EXPECT_THAT(HMCgetdatainfo(fid, 9999, 9999, coord, 0, 1, &off, &len),
              Eq(FAIL));

  // Non-chunked element (line 2007)
  const int32_t aid = Hstartwrite(fid, 1340, 1, 4);
  ASSERT_THAT(aid, Ne(FAIL));
  uint8_t data[4] = {1, 2, 3, 4};
  EXPECT_THAT(Hwrite(aid, 4, data), Eq(4));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  EXPECT_THAT(HMCgetdatainfo(fid, 1340, 1, coord, 0, 1, &off, &len), Eq(FAIL));
}

TEST(HchunksTest, HMCwriteChunkOnReadOnlyFileFails) {
  const std::string path = GetTempFilePath("hchunks_ro_chunk.hdf");
  {
    const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_THAT(fid, Ne(FAIL));
    DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
    HCHUNK_DEF chk = {
        .chunk_size = 2,
        .nt_size = 1,
        .num_dims = 1,
        .pdims = &dim,
        .chunk_flag = 0,
        .comp_type = COMP_CODE_NONE,
        .model_type = COMP_MODEL_STDIO,
        .cinfo = nullptr,
        .minfo = nullptr,
    };
    uint8_t fill = 0;
    const int32_t aid = HMCcreate(fid, 1250, 1, 1, 1, &fill, &chk);
    ASSERT_THAT(aid, Ne(FAIL));
    EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
    EXPECT_THAT(Hclose(fid), Eq(SUCCEED));
  }

  const int32_t fid = Hopen(path.c_str(), DFACC_READ, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  const int32_t aid = Hstartread(fid, 1250, 1);
  ASSERT_THAT(aid, Ne(FAIL));

  int32_t origin[1] = {0};
  uint8_t data[2] = {1, 2};
  // Exercises line 3080: writeChunk on read-only file fails
  EXPECT_THAT(HMCwriteChunk(aid, origin, data), Eq(FAIL));

  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
}

TEST(HchunksTest, HMCreadChunkOnWriteOnlyFileFails) {
  const std::string path = GetTempFilePath("hchunks_wo_chunk.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 2,
      .nt_size = 1,
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill = 0;
  const int32_t aid = HMCcreate(fid, 1255, 1, 1, 1, &fill, &chk);
  ASSERT_THAT(aid, Ne(FAIL));

  // Change file access to write-only without DFACC_READ to exercise line 2706
  auto* arec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_THAT(arec, Ne(nullptr));
  auto* frec = static_cast<filerec_t*>(HAatom_object(arec->file_id));
  ASSERT_THAT(frec, Ne(nullptr));
  const uint32_t orig_access = frec->access;
  frec->access = DFACC_WRITE;

  int32_t origin[1] = {0};
  uint8_t data[2] = {0};
  EXPECT_THAT(HMCreadChunk(aid, origin, data), Eq(FAIL));

  frec->access = orig_access;
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
}

TEST(HchunksTest, HreadLengthZeroReadsRemaining) {
  const std::string path = GetTempFilePath("hchunks_read_zero.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 2,
      .nt_size = 1,
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill = 0;
  const int32_t aid = HMCcreate(fid, 1280, 1, 1, 1, &fill, &chk);
  ASSERT_THAT(aid, Ne(FAIL));
  uint8_t write_buf[4] = {11, 22, 33, 44};
  EXPECT_THAT(Hwrite(aid, 4, write_buf), Eq(4));

  EXPECT_THAT(Hseek(aid, 0, DF_START), Eq(SUCCEED));

  // Hread with length == 0: exercises line 2814 (reads whole remaining length)
  uint8_t read_buf[4] = {0};
  EXPECT_THAT(Hread(aid, 0, read_buf), Eq(4));
  EXPECT_THAT(read_buf[0], Eq(11));
  EXPECT_THAT(read_buf[3], Eq(44));

  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));
}

TEST(HchunksTest, ChunkedFuncsNullAccrecAndSpecialBranches) {
  // Test chunked_funcs with nullptr
  EXPECT_THAT(chunked_funcs.stread(nullptr), Eq(FAIL));
  EXPECT_THAT(chunked_funcs.stwrite(nullptr), Eq(FAIL));
  uint8_t buf[2] = {0};
  EXPECT_THAT(chunked_funcs.read(nullptr, 0, buf), Eq(FAIL));
  EXPECT_THAT(chunked_funcs.write(nullptr, 0, buf), Eq(FAIL));
  sp_info_block_t sp_info = {};
  EXPECT_THAT(chunked_funcs.info(nullptr, &sp_info), Eq(FAIL));

  // HMCPinfo on accrec with null special_info (exercises line 3651)
  accrec_t non_chk = {};
  non_chk.special_info = nullptr;
  EXPECT_THAT(chunked_funcs.info(&non_chk, &sp_info), Eq(FAIL));

  // HMCPendaccess on accrec with bad file_id (exercises lines 3477, 3496, 3497)
  accrec_t* arec = HIget_access_rec();
  ASSERT_THAT(arec, Ne(nullptr));
  arec->file_id = FAIL;
  EXPECT_THAT(chunked_funcs.endaccess(arec), Eq(FAIL));

  // HMCsetMaxcache with special == SPECIAL_CHUNKED but special_info == nullptr
  // (exercises line 2446)
  accrec_t* arec2 = HIget_access_rec();
  ASSERT_THAT(arec2, Ne(nullptr));
  arec2->special = SPECIAL_CHUNKED;
  arec2->special_info = nullptr;
  const int32_t mock_aid = HAregister_atom(AIDGROUP, arec2);
  ASSERT_THAT(mock_aid, Ne(FAIL));
  EXPECT_THAT(HMCsetMaxcache(mock_aid, 5, 0), Eq(FAIL));
  HAremove_atom(mock_aid);
  HIrelease_accrec_node(arec2);
}

TEST(HchunksTest, HMCgetcomptypeOnCompressedAndUncompressed) {
  const std::string path = GetTempFilePath("hchunks_getcomptype.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  // 1. Uncompressed
  DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 2,
      .nt_size = 1,
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill = 0;
  const int32_t aid1 = HMCcreate(fid, 1290, 1, 1, 1, &fill, &chk);
  ASSERT_THAT(aid1, Ne(FAIL));
  EXPECT_THAT(Hendaccess(aid1), Eq(SUCCEED));

  comp_coder_t coder1 = COMP_CODE_INVALID;
  EXPECT_THAT(HCPgetcomptype(fid, 1290, 1, &coder1), Eq(SUCCEED));
  EXPECT_THAT(coder1, Eq(COMP_CODE_NONE));

  // 2. Compressed (Deflate)
  comp_info c_info = {};
  c_info.deflate_setting.level = 6;
  model_info m_info = {};
  HCHUNK_DEF comp_chk = {
      .chunk_size = 2,
      .nt_size = 1,
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = SPECIAL_COMP,
      .comp_type = COMP_CODE_DEFLATE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = &c_info,
      .minfo = &m_info,
  };
  const int32_t aid2 = HMCcreate(fid, 1290, 2, 1, 1, &fill, &comp_chk);
  ASSERT_THAT(aid2, Ne(FAIL));
  EXPECT_THAT(Hendaccess(aid2), Eq(SUCCEED));

  comp_coder_t coder2 = COMP_CODE_INVALID;
  EXPECT_THAT(HCPgetcomptype(fid, 1290, 2, &coder2), Eq(SUCCEED));
  EXPECT_THAT(coder2, Eq(COMP_CODE_DEFLATE));
}

TEST(HchunksTest, HMCgetdatainfoZeroInfoCountAndNullArrays) {
  const std::string path = GetTempFilePath("hchunks_datainfo_zero.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 2,
      .nt_size = 1,
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill = 0;
  const int32_t aid = HMCcreate(fid, 1260, 1, 1, 1, &fill, &chk);
  ASSERT_THAT(aid, Ne(FAIL));
  int32_t origin[1] = {0};
  uint8_t d[2] = {10, 20};
  EXPECT_THAT(HMCwriteChunk(aid, origin, d), Eq(2));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  int32_t off = 0;
  int32_t len = 0;
  // info_count == 0 with non-null arrays: exercises line 1987 (DFE_ARGS)
  EXPECT_THAT(HMCgetdatainfo(fid, 1260, 1, origin, 0, 0, &off, &len), Eq(FAIL));
  // Null arrays with info_count = 1: exercises line 2038
  EXPECT_THAT(HMCgetdatainfo(fid, 1260, 1, origin, 0, 1, nullptr, nullptr),
              Eq(1));
}

TEST(HchunksTest, HCPgetdatasizeNullPtrPermutations) {
  const std::string path = GetTempFilePath("hchunks_datasize_nulls.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_THAT(fid, Ne(FAIL));
  absl::Cleanup close_file = [fid] { EXPECT_THAT(Hclose(fid), Eq(SUCCEED)); };

  DIM_DEF dim = {.dim_length = 4, .chunk_length = 2, .distrib_type = 0};
  HCHUNK_DEF chk = {
      .chunk_size = 2,
      .nt_size = 1,
      .num_dims = 1,
      .pdims = &dim,
      .chunk_flag = 0,
      .comp_type = COMP_CODE_NONE,
      .model_type = COMP_MODEL_STDIO,
      .cinfo = nullptr,
      .minfo = nullptr,
  };
  uint8_t fill = 0;
  const int32_t aid = HMCcreate(fid, 1270, 1, 1, 1, &fill, &chk);
  ASSERT_THAT(aid, Ne(FAIL));
  int32_t origin[1] = {0};
  uint8_t d[2] = {1, 2};
  EXPECT_THAT(HMCwriteChunk(aid, origin, d), Eq(2));
  EXPECT_THAT(Hendaccess(aid), Eq(SUCCEED));

  int32_t comp_size = 0;
  int32_t orig_size = 0;
  EXPECT_THAT(HCPgetdatasize(fid, 1270, 1, &comp_size, nullptr), Eq(SUCCEED));
  EXPECT_THAT(HCPgetdatasize(fid, 1270, 1, nullptr, &orig_size), Eq(SUCCEED));
  EXPECT_THAT(HCPgetdatasize(fid, 1270, 1, nullptr, nullptr), Eq(SUCCEED));
}

// TODO: b/383748289 - Increase coverage for remaining uncovered error paths in
// hchunks.c.

}  // namespace
}  // namespace hdf4
