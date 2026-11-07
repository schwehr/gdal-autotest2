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

#include <cstdint>
#include <filesystem>  // NOLINT
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf4/hdf/src/atom_priv.h"
#include "third_party/hdf4/hdf/src/hdf.h"
// clang-format off
#include "third_party/hdf4/hdf/src/hcomp.h"
// clang-format on
#include "third_party/hdf4/hdf/src/hfile_priv.h"

namespace hdf4 {
namespace {

std::string GetTempFilePath(const std::string& name) {
  return (std::filesystem::path(::testing::TempDir()) / name).string();
}

TEST(HcompriTest, ConvertArgumentValidation) {
  const std::string path = GetTempFilePath("hcompri_args.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    Hclose(fid);
    std::filesystem::remove(path);
  };

  comp_info cinfo{};

  // Invalid file ID.
  HEclear();
  EXPECT_EQ(FAIL,
            HRPconvert(FAIL, DFTAG_RI, 100, 10, 10, DFTAG_RLE, &cinfo, 1));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // Special tag passed as tag parameter (SPECIALTAG macro is true).
  HEclear();
  EXPECT_EQ(FAIL, HRPconvert(fid, MKSPECIALTAG(DFTAG_RI), 100, 10, 10,
                             DFTAG_RLE, &cinfo, 1));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
}

TEST(HcompriTest, StreadAndStwriteStubs) {
#if defined(NDEBUG)
  EXPECT_EQ(FAIL, HRPstread(nullptr));
  EXPECT_EQ(FAIL, HRPstwrite(nullptr));
#else
  EXPECT_DEATH(HRPstread(nullptr), "Should never be called");
  EXPECT_DEATH(HRPstwrite(nullptr), "Should never be called");
#endif
}

TEST(HcompriTest, SeekOperations) {
  const std::string path = GetTempFilePath("hcompri_seek.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    Hclose(fid);
    std::filesystem::remove(path);
  };

  comp_info cinfo{};
  const int32_t aid =
      HRPconvert(fid, DFTAG_RI, 101, 8, 8, DFTAG_RLE, &cinfo, 1);
  ASSERT_NE(aid, FAIL);
  absl::Cleanup aid_cleanup = [&]() { Hendaccess(aid); };

  // Only origin == DF_START and offset == 0 are allowed.
  EXPECT_EQ(SUCCEED, Hseek(aid, 0, DF_START));

  // Invalid offsets or origins.
  HEclear();
  EXPECT_EQ(FAIL, Hseek(aid, 1, DF_START));
  EXPECT_EQ(DFE_RANGE, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, Hseek(aid, -1, DF_START));
  EXPECT_EQ(DFE_RANGE, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, Hseek(aid, 0, DF_CURRENT));
  EXPECT_EQ(DFE_RANGE, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, Hseek(aid, 0, DF_END));
  EXPECT_EQ(DFE_RANGE, HEvalue(1));
}

TEST(HcompriTest, ReadWriteRoundTripRle) {
  const std::string path = GetTempFilePath("hcompri_rw_rle.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    Hclose(fid);
    std::filesystem::remove(path);
  };

  constexpr int32_t kXDim = 8;
  constexpr int32_t kYDim = 8;
  constexpr int32_t kImageSize = kXDim * kYDim;
  std::vector<uint8_t> write_data(kImageSize);
  for (size_t i = 0; i < write_data.size(); ++i) {
    write_data[i] = static_cast<uint8_t>(i % 16);
  }

  comp_info cinfo{};
  const int32_t aid =
      HRPconvert(fid, DFTAG_RI, 102, kXDim, kYDim, DFTAG_RLE, &cinfo, 1);
  ASSERT_NE(aid, FAIL);

  // Write invalid length (not 0 and not image_size).
  HEclear();
  EXPECT_EQ(FAIL, Hwrite(aid, kImageSize - 1, write_data.data()));
  EXPECT_EQ(DFE_RANGE, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, Hwrite(aid, kImageSize + 1, write_data.data()));
  EXPECT_EQ(DFE_RANGE, HEvalue(1));

  // Write with exact image_size.
  EXPECT_EQ(kImageSize, Hwrite(aid, kImageSize, write_data.data()));

  EXPECT_EQ(SUCCEED, Hendaccess(aid));

  // Now reopen existing tag/ref with HRPconvert (tests Hexist >= 0 path).
  const int32_t aid2 =
      HRPconvert(fid, DFTAG_RI, 102, kXDim, kYDim, DFTAG_RLE, &cinfo, 1);
  ASSERT_NE(aid2, FAIL);

  // Read invalid length (not 0 and not image_size).
  std::vector<uint8_t> read_data(kImageSize, 0);
  HEclear();
  EXPECT_EQ(FAIL, Hread(aid2, 5, read_data.data()));
  EXPECT_EQ(DFE_RANGE, HEvalue(1));

  // Read with exact image_size.
  EXPECT_EQ(kImageSize, Hread(aid2, kImageSize, read_data.data()));
  EXPECT_EQ(write_data, read_data);

  // Read with length == 0 (triggers length = info->image_size branch).
  std::vector<uint8_t> read_data0(kImageSize, 0);
  EXPECT_EQ(kImageSize, Hread(aid2, 0, read_data0.data()));
  EXPECT_EQ(write_data, read_data0);

  // Write with length == 0 (triggers length = info->image_size branch).
  EXPECT_EQ(kImageSize, Hwrite(aid2, 0, write_data.data()));

  EXPECT_EQ(SUCCEED, Hendaccess(aid2));
}

TEST(HcompriTest, InquireAndInfo) {
  const std::string path = GetTempFilePath("hcompri_inquire.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    Hclose(fid);
    std::filesystem::remove(path);
  };

  constexpr int32_t kXDim = 4;
  constexpr int32_t kYDim = 4;
  constexpr int32_t kImageSize = kXDim * kYDim;
  comp_info cinfo{};

  // New element: inquire should report length == -1.
  const int32_t aid =
      HRPconvert(fid, DFTAG_RI, 103, kXDim, kYDim, DFTAG_RLE, &cinfo, 1);
  ASSERT_NE(aid, FAIL);

  int32_t file_id = 0;
  uint16_t tag = 0;
  uint16_t ref = 0;
  int32_t length = 0;
  int32_t offset = 0;
  int32_t posn = -1;
  int16_t access = 0;
  int16_t special = 0;

  EXPECT_EQ(SUCCEED, Hinquire(aid, &file_id, &tag, &ref, &length, &offset,
                              &posn, &access, &special));
  EXPECT_EQ(file_id, fid);
  EXPECT_EQ(tag, DFTAG_RI);
  EXPECT_EQ(ref, 103);
  EXPECT_EQ(length, -1);
  EXPECT_EQ(posn, 0);
  EXPECT_EQ(access, DFACC_RDWR);
  EXPECT_EQ(special, SPECIAL_COMPRAS);

  // Write data so it is no longer a new element, then close and reopen.
  std::vector<uint8_t> data(kImageSize, 42);
  EXPECT_EQ(kImageSize, Hwrite(aid, kImageSize, data.data()));
  EXPECT_EQ(SUCCEED, Hendaccess(aid));

  const int32_t aid2 =
      HRPconvert(fid, DFTAG_RI, 103, kXDim, kYDim, DFTAG_RLE, &cinfo, 1);
  ASSERT_NE(aid2, FAIL);
  EXPECT_EQ(SUCCEED, Hinquire(aid2, &file_id, &tag, &ref, &length, &offset,
                              &posn, &access, &special));
  EXPECT_EQ(length, kImageSize);

  // Inquire with NULL pointers should succeed without crashing.
  EXPECT_EQ(SUCCEED, Hinquire(aid2, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr));

  // Test HRPinfo.
  auto* access_rec = static_cast<accrec_t*>(HAatom_object(aid2));
  ASSERT_NE(access_rec, nullptr);

  sp_info_block_t sp_info{};
  EXPECT_EQ(SUCCEED, HRPinfo(access_rec, &sp_info));
  EXPECT_EQ(sp_info.key, SPECIAL_COMPRAS);

  // HRPinfo with non-matching special returns FAIL.
  access_rec->special = 0;
  HEclear();
  EXPECT_EQ(FAIL, HRPinfo(access_rec, &sp_info));
  EXPECT_EQ(DFE_INTERNAL, HEvalue(1));
  access_rec->special = SPECIAL_COMPRAS;

  // HRPinquire failure path when ddid is invalid.
  const int32_t saved_ddid = access_rec->ddid;
  access_rec->ddid = FAIL;
  HEclear();
  EXPECT_EQ(FAIL, Hinquire(aid2, &file_id, &tag, &ref, &length, &offset, &posn,
                           &access, &special));
  EXPECT_EQ(DFE_INTERNAL, HEvalue(1));
  access_rec->ddid = saved_ddid;

  EXPECT_EQ(SUCCEED, Hendaccess(aid2));
}

TEST(HcompriTest, EndAccessAndCloseAidEdgeCases) {
  const std::string path = GetTempFilePath("hcompri_endaccess.hdf");
  const int32_t fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(fid, FAIL);
  absl::Cleanup file_cleanup = [&]() {
    Hclose(fid);
    std::filesystem::remove(path);
  };

  // HRPendaccess with nullptr argument.
  HEclear();
  EXPECT_EQ(FAIL, HRPendaccess(nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // HRPendaccess with an allocated access record that has a bad file_id.
  accrec_t* bad_rec = HIget_access_rec();
  ASSERT_NE(bad_rec, nullptr);
  bad_rec->file_id = FAIL;
  HEclear();
  EXPECT_EQ(FAIL, HRPendaccess(bad_rec));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // Normal creation and close AID with attached > 1.
  comp_info cinfo{};
  const int32_t aid =
      HRPconvert(fid, DFTAG_RI, 104, 4, 4, DFTAG_RLE, &cinfo, 1);
  ASSERT_NE(aid, FAIL);

  auto* access_rec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_NE(access_rec, nullptr);

  auto* info = static_cast<int*>(access_rec->special_info);
  ASSERT_NE(info, nullptr);
  *info = 2;  // Set attached = 2
  EXPECT_EQ(SUCCEED, HRPcloseAID(access_rec));
  EXPECT_EQ(*info, 1);
  EXPECT_NE(access_rec->special_info, nullptr);

  // Normal Hendaccess closes the AID, decrements attached from 1 to 0, frees
  // info and accrec.
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
}

}  // namespace
}  // namespace hdf4
