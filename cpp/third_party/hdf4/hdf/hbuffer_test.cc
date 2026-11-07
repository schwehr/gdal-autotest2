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

// Tests for buffered data elements (third_party/hdf4/hdf/src/hbuffer.c).
//
// See also:
//   https://github.com/HDFGroup/hdf4/blob/master/hdf/src/hbuffer.c

#include <cstdint>
#include <cstring>
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

#include "third_party/hdf4/hdf/src/hdf.h"
#include "third_party/hdf4/hdf/src/herr_priv.h"
#include "third_party/hdf4/hdf/src/hfile_priv.h"

namespace hdf4 {
namespace {

using ::testing::ElementsAre;

// Helper to create a temp file path inside ::testing::TempDir().
std::string GetTempFilePath(const std::string& name) {
  return (std::filesystem::path(::testing::TempDir()) / name).string();
}

TEST(HbufferTest, ConvertInvalidAidFails) {
  HEclear();
  EXPECT_EQ(FAIL, HBconvert(FAIL));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, HBconvert(0));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, HBconvert(999999));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));
}

TEST(HbufferTest, ReadBufferedElement) {
  const std::string path = GetTempFilePath("hbuffer_read.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  const std::vector<uint8_t> original_data = {10, 20, 30, 40, 50, 60, 70, 80};
  constexpr uint16 kTag = 1000;
  constexpr uint16 kRef = 1;

  ASSERT_GT(
      Hputelement(fid, kTag, kRef, original_data.data(), original_data.size()),
      0);

  const int32 aid = Hstartread(fid, kTag, kRef);
  ASSERT_NE(FAIL, aid);

  // Convert to buffered element.
  EXPECT_EQ(SUCCEED, HBconvert(aid));

  // Inquire all fields.
  int32 file_id = FAIL;
  uint16 tag = 0;
  uint16 ref = 0;
  int32 length = 0;
  int32 offset = 0;
  int32 posn = 0;
  int16 access = 0;
  int16 special = 0;
  EXPECT_EQ(SUCCEED, Hinquire(aid, &file_id, &tag, &ref, &length, &offset,
                              &posn, &access, &special));
  EXPECT_EQ(fid, file_id);
  EXPECT_EQ(kTag, tag);
  EXPECT_EQ(kRef, ref);
  EXPECT_EQ(original_data.size(), length);
  EXPECT_GT(offset, 0);
  EXPECT_EQ(original_data.size(), posn);
  EXPECT_EQ(DFACC_READ, access);
  EXPECT_EQ(SPECIAL_BUFFERED, special);

  // Inquire with null pointers.
  EXPECT_EQ(SUCCEED, Hinquire(aid, nullptr, nullptr, nullptr, nullptr, nullptr,
                              nullptr, nullptr, nullptr));

  // Seek and read.
  EXPECT_EQ(SUCCEED, Hseek(aid, 2, DF_START));
  std::vector<uint8_t> read_buf(4, 0);
  EXPECT_EQ(4, Hread(aid, 4, read_buf.data()));
  EXPECT_THAT(read_buf, ElementsAre(30, 40, 50, 60));

  // Read with length 0 reads until end of element.
  std::vector<uint8_t> rest_buf(10, 0);
  EXPECT_EQ(2, Hread(aid, 0, rest_buf.data()));
  EXPECT_EQ(70, rest_buf[0]);
  EXPECT_EQ(80, rest_buf[1]);

  // Read when posn + length > info->length clips to remaining bytes.
  EXPECT_EQ(SUCCEED, Hseek(aid, 5, DF_START));
  std::vector<uint8_t> clip_buf(10, 0);
  EXPECT_EQ(3, Hread(aid, 10, clip_buf.data()));
  EXPECT_EQ(60, clip_buf[0]);
  EXPECT_EQ(70, clip_buf[1]);
  EXPECT_EQ(80, clip_buf[2]);

  // Negative read length returns FAIL.
  HEclear();
  EXPECT_EQ(FAIL, Hread(aid, -1, clip_buf.data()));
  EXPECT_EQ(DFE_RANGE, HEvalue(1));

  // Special info.
  sp_info_block_t sp_info{};
  EXPECT_EQ(SUCCEED, HDget_special_info(aid, &sp_info));
  EXPECT_EQ(SPECIAL_BUFFERED, sp_info.key);
  EXPECT_NE(FAIL, sp_info.buf_aid);

  // Inquire failure when underlying DD ID is invalid.
  auto* buf_accrec = static_cast<accrec_t*>(HAatom_object(sp_info.buf_aid));
  ASSERT_NE(nullptr, buf_accrec);
  const int32 orig_ddid = buf_accrec->ddid;
  buf_accrec->ddid = FAIL;
  HEclear();
  EXPECT_EQ(FAIL, Hinquire(aid, nullptr, nullptr, nullptr, nullptr, nullptr,
                           nullptr, nullptr, nullptr));
  EXPECT_EQ(DFE_INTERNAL, HEvalue(1));

  // HBconvert failure on already-special element when inquire fails.
  HEclear();
  EXPECT_EQ(FAIL, HBconvert(aid));
  EXPECT_EQ(DFE_INTERNAL, HEvalue(1));

  buf_accrec->ddid = orig_ddid;

  // Test Convert failure when HTPinquire fails on regular element.
  const int32 aid_bad = Hstartread(fid, kTag, kRef);
  ASSERT_NE(FAIL, aid_bad);
  auto* accrec = static_cast<accrec_t*>(HAatom_object(aid_bad));
  ASSERT_NE(nullptr, accrec);
  const int32 saved_ddid = accrec->ddid;
  accrec->ddid = FAIL;
  HEclear();
  EXPECT_EQ(FAIL, HBconvert(aid_bad));
  EXPECT_EQ(DFE_INTERNAL, HEvalue(1));
  accrec->ddid = saved_ddid;
  EXPECT_EQ(SUCCEED, Hendaccess(aid_bad));

  EXPECT_EQ(SUCCEED, Hendaccess(aid));
}

TEST(HbufferTest, SeekVariations) {
  const std::string path = GetTempFilePath("hbuffer_seek.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  const std::vector<uint8_t> data(20, 42);
  constexpr uint16 kTag = 1001;
  constexpr uint16 kRef = 2;
  ASSERT_GT(Hputelement(fid, kTag, kRef, data.data(), data.size()), 0);

  const int32 aid = Hstartread(fid, kTag, kRef);
  ASSERT_NE(FAIL, aid);
  absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

  ASSERT_EQ(SUCCEED, HBconvert(aid));

  // DF_START
  EXPECT_EQ(SUCCEED, Hseek(aid, 5, DF_START));
  int32 posn = 0;
  EXPECT_EQ(SUCCEED, Hinquire(aid, nullptr, nullptr, nullptr, nullptr, nullptr,
                              &posn, nullptr, nullptr));
  EXPECT_EQ(5, posn);

  // DF_CURRENT
  EXPECT_EQ(SUCCEED, Hseek(aid, 3, DF_CURRENT));
  EXPECT_EQ(SUCCEED, Hinquire(aid, nullptr, nullptr, nullptr, nullptr, nullptr,
                              &posn, nullptr, nullptr));
  EXPECT_EQ(8, posn);

  // DF_END
  EXPECT_EQ(SUCCEED, Hseek(aid, -4, DF_END));
  EXPECT_EQ(SUCCEED, Hinquire(aid, nullptr, nullptr, nullptr, nullptr, nullptr,
                              &posn, nullptr, nullptr));
  EXPECT_EQ(16, posn);

  // Negative seek results in DFE_RANGE error.
  HEclear();
  EXPECT_EQ(FAIL, Hseek(aid, -1, DF_START));
  EXPECT_EQ(DFE_RANGE, HEvalue(1));

  HEclear();
  EXPECT_EQ(FAIL, Hseek(aid, -25, DF_CURRENT));
  EXPECT_EQ(DFE_RANGE, HEvalue(1));
}

TEST(HbufferTest, StreadAndStwriteStubs) {
#if defined(NDEBUG)
  EXPECT_EQ(FAIL, HBPstread(nullptr));
  EXPECT_EQ(FAIL, HBPstwrite(nullptr));
#else
  EXPECT_DEATH(HBPstread(nullptr), "Should never be called");
  EXPECT_DEATH(HBPstwrite(nullptr), "Should never be called");
#endif
}

TEST(HbufferTest, WriteAndFlushToDisk) {
  const std::string path = GetTempFilePath("hbuffer_write.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  constexpr uint16 kTag = 1002;
  constexpr uint16 kRef = 1;

  // 1. Create file and write buffered element starting from unallocated data.
  {
    const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    // Hstartaccess creates a new DD entry with INVALID_OFFSET and
    // INVALID_LENGTH.
    const int32 aid = Hstartaccess(fid, kTag, kRef, DFACC_RDWR);
    ASSERT_NE(FAIL, aid);

    ASSERT_EQ(SUCCEED, Happendable(aid));

    // Convert unwritten element to buffered (exercises data_off == -1 and
    // data_len == -1 branch).
    ASSERT_EQ(SUCCEED, HBconvert(aid));

    // Negative write length returns FAIL.
    const uint8_t byte = 0xAA;
    HEclear();
    EXPECT_EQ(FAIL, Hwrite(aid, -1, &byte));
    EXPECT_EQ(DFE_RANGE, HEvalue(1));

    // First write allocates buffer when info->buf == NULL.
    const std::vector<uint8_t> chunk1 = {1, 2, 3, 4};
    EXPECT_EQ(4, Hwrite(aid, chunk1.size(), chunk1.data()));

    // Second write expands buffer using realloc.
    const std::vector<uint8_t> chunk2 = {5, 6, 7, 8, 9, 10};
    EXPECT_EQ(6, Hwrite(aid, chunk2.size(), chunk2.data()));

    // Overwrite without expanding buffer.
    EXPECT_EQ(SUCCEED, Hseek(aid, 1, DF_START));
    const std::vector<uint8_t> chunk3 = {20, 30};
    EXPECT_EQ(2, Hwrite(aid, chunk3.size(), chunk3.data()));

    // Close and flush to disk.
    // Test Convert failure when HTPinquire fails on regular element.
    const int32 aid_bad = Hstartread(fid, kTag, kRef);
    ASSERT_NE(FAIL, aid_bad);
    auto* accrec = static_cast<accrec_t*>(HAatom_object(aid_bad));
    ASSERT_NE(nullptr, accrec);
    const int32 saved_ddid = accrec->ddid;
    accrec->ddid = FAIL;
    HEclear();
    EXPECT_EQ(FAIL, HBconvert(aid_bad));
    EXPECT_EQ(DFE_INTERNAL, HEvalue(1));
    accrec->ddid = saved_ddid;
    EXPECT_EQ(SUCCEED, Hendaccess(aid_bad));

    EXPECT_EQ(SUCCEED, Hendaccess(aid));
  }

  // 2. Re-open and verify written content on disk.
  {
    const int32 fid = Hopen(path.c_str(), DFACC_READ, 0);
    ASSERT_NE(FAIL, fid);
    absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

    const int32 aid = Hstartread(fid, kTag, kRef);
    ASSERT_NE(FAIL, aid);
    absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

    int32 length = 0;
    EXPECT_EQ(SUCCEED, Hinquire(aid, nullptr, nullptr, nullptr, &length,
                                nullptr, nullptr, nullptr, nullptr));
    EXPECT_EQ(10, length);

    std::vector<uint8_t> read_buf(10, 0);
    EXPECT_EQ(10, Hread(aid, 10, read_buf.data()));
    EXPECT_THAT(read_buf, ElementsAre(1, 20, 30, 4, 5, 6, 7, 8, 9, 10));
  }
}

TEST(HbufferTest, ConvertSpecialElementAndDoubleConvert) {
  const std::string path = GetTempFilePath("hbuffer_special.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  const std::vector<uint8_t> data = {1, 2, 3, 4, 5, 6, 7, 8};
  constexpr uint16 kTag = 1003;
  constexpr uint16 kRef = 1;

  ASSERT_GT(Hputelement(fid, kTag, kRef, data.data(), data.size()), 0);

  const int32 aid = Hstartwrite(fid, kTag, kRef, data.size());
  ASSERT_NE(FAIL, aid);
  absl::Cleanup close_aid = [aid] { EXPECT_EQ(SUCCEED, Hendaccess(aid)); };

  // First conversion.
  ASSERT_EQ(SUCCEED, HBconvert(aid));

  // Second conversion exercises the access_rec->special != 0 branch.
  EXPECT_EQ(SUCCEED, HBconvert(aid));
}

TEST(HbufferTest, CloseAidWithMultipleAttached) {
  const std::string path = GetTempFilePath("hbuffer_attached.hdf");
  std::filesystem::remove(path);
  absl::Cleanup remove_file = [&] { std::filesystem::remove(path); };

  const int32 fid = Hopen(path.c_str(), DFACC_CREATE, 0);
  ASSERT_NE(FAIL, fid);
  absl::Cleanup close_file = [fid] { EXPECT_EQ(SUCCEED, Hclose(fid)); };

  const std::vector<uint8_t> data = {1, 2, 3, 4};
  constexpr uint16 kTag = 1005;
  constexpr uint16 kRef = 1;

  ASSERT_GT(Hputelement(fid, kTag, kRef, data.data(), data.size()), 0);

  const int32 aid = Hstartread(fid, kTag, kRef);
  ASSERT_NE(FAIL, aid);

  ASSERT_EQ(SUCCEED, HBconvert(aid));

  accrec_t* access_rec = static_cast<accrec_t*>(HAatom_object(aid));
  ASSERT_NE(nullptr, access_rec);

  // Simulate an extra reference to the special info structure.
  // The first call to HBPcloseAID should decrement attached from 2 to 1
  // without releasing the buffer.
  int* attached_ptr = static_cast<int*>(access_rec->special_info);
  *attached_ptr = 2;

  EXPECT_EQ(SUCCEED, HBPcloseAID(access_rec));
  EXPECT_EQ(1, *attached_ptr);

  // Final Hendaccess will decrement attached from 1 to 0 and free everything.
  EXPECT_EQ(SUCCEED, Hendaccess(aid));
}

TEST(HbufferTest, InternalErrorPaths) {
  // Test HBPendaccess with null pointer.
  HEclear();
  EXPECT_EQ(FAIL, HBPendaccess(nullptr));
  EXPECT_EQ(DFE_ARGS, HEvalue(1));

  // Test HBPinfo with invalid special tag.
  accrec_t dummy_accrec{};
  dummy_accrec.special = 0;
  sp_info_block_t info_block{};
  HEclear();
  EXPECT_EQ(FAIL, HBPinfo(&dummy_accrec, &info_block));
  EXPECT_EQ(DFE_INTERNAL, HEvalue(1));
}

}  // namespace
}  // namespace hdf4
