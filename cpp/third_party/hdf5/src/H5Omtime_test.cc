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
// Tests legacy (1.6.0 ASCII) and modern (1.6.1+ binary) object modification
// time message encoding, decoding, validation, copying, sizing, freeing,
// debugging, and object-header integration in third_party/hdf5/src/H5Omtime.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Omtime.c

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

// `:hdf5` is built with -DNDEBUG, omitting debug fields from cache/object
// header structs pulled in via H5Opkg.h. Define NDEBUG around internal headers
// so struct layouts match `:hdf5`.
#ifndef NDEBUG
#define H5OMTIME_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#define H5O_FRIEND
#include "third_party/hdf5/src/H5CXprivate.h"
#include "third_party/hdf5/src/H5Opkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#ifdef H5OMTIME_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5OMTIME_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

class ScopedApiContext {
 public:
  ScopedApiContext() {
    if (H5CX_push(&node_) >= 0) {
      pushed_ = true;
    }
  }

  ~ScopedApiContext() {
    if (pushed_) {
      H5CX_pop(/*update_dxpl_props=*/false);
    }
  }

  ScopedApiContext(const ScopedApiContext&) = delete;
  ScopedApiContext& operator=(const ScopedApiContext&) = delete;

 private:
  H5CX_node_t node_{};
  bool pushed_ = false;
};

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

std::string ReadTmpFile(FILE* fp) {
  std::fflush(fp);
  std::rewind(fp);
  std::string out;
  char chunk[256];
  while (true) {
    const size_t n = std::fread(chunk, 1, sizeof(chunk), fp);
    if (n == 0) {
      break;
    }
    out.append(chunk, n);
  }
  return out;
}

TEST(H5OmtimeTest, DescriptorsMatchClassSpecs) {
  ScopedFile file = CreateInMemoryFile("mtime_desc.h5");
  ASSERT_TRUE(file.valid());

  EXPECT_EQ(H5O_MSG_MTIME[0].id, static_cast<unsigned>(H5O_MTIME_ID));
  EXPECT_STREQ(H5O_MSG_MTIME[0].name, "mtime");
  EXPECT_EQ(H5O_MSG_MTIME[0].native_size, sizeof(time_t));
  EXPECT_EQ(H5O_MSG_MTIME[0].reset, nullptr);
  EXPECT_NE(H5O_MSG_MTIME[0].free, nullptr);
  EXPECT_EQ(H5O_msg_class_g[H5O_MTIME_ID], H5O_MSG_MTIME);

  EXPECT_EQ(H5O_MSG_MTIME_NEW[0].id, static_cast<unsigned>(H5O_MTIME_NEW_ID));
  EXPECT_STREQ(H5O_MSG_MTIME_NEW[0].name, "mtime_new");
  EXPECT_EQ(H5O_MSG_MTIME_NEW[0].native_size, sizeof(time_t));
  EXPECT_EQ(H5O_MSG_MTIME_NEW[0].reset, nullptr);
  EXPECT_NE(H5O_MSG_MTIME_NEW[0].free, nullptr);
  EXPECT_EQ(H5O_msg_class_g[H5O_MTIME_NEW_ID], H5O_MSG_MTIME_NEW);
}

TEST(H5OmtimeTest, NewMtimeEncodeDecodeValidationCopyAndDebug) {
  ScopedFile file = CreateInMemoryFile("mtime_new_codec.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  const time_t kTimestamp = 1704164645;  // 2024-01-02 03:04:05 UTC
  EXPECT_EQ(H5O_msg_raw_size(f, H5O_MTIME_NEW_ID, false, &kTimestamp), 8U);

  std::array<uint8_t, 8> encoded{};
  ASSERT_GE(
      H5O_msg_encode(f, H5O_MTIME_NEW_ID, false, encoded.data(), &kTimestamp),
      0);
  EXPECT_EQ(encoded[0], 1);
  EXPECT_EQ(encoded[1], 0);
  EXPECT_EQ(encoded[2], 0);
  EXPECT_EQ(encoded[3], 0);

  time_t* decoded = static_cast<time_t*>(H5O_msg_decode(
      f, nullptr, H5O_MTIME_NEW_ID, encoded.size(), encoded.data()));
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(*decoded, kTimestamp);
  H5O_msg_free(H5O_MTIME_NEW_ID, decoded);

  // Decode boundary / error cases.
  {
    ScopedErrorSilence silence;
    const uint8_t kEmptyGuard[1] = {0};
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MTIME_NEW_ID, 0, kEmptyGuard),
              nullptr);

    std::array<uint8_t, 8> bad_ver = encoded;
    bad_ver[0] = 2;
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MTIME_NEW_ID, bad_ver.size(),
                             bad_ver.data()),
              nullptr);
    bad_ver[0] = 0;
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MTIME_NEW_ID, bad_ver.size(),
                             bad_ver.data()),
              nullptr);

    // Truncated in reserved bytes and in 4-byte timestamp.
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MTIME_NEW_ID, 2, encoded.data()),
              nullptr);
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MTIME_NEW_ID, 6, encoded.data()),
              nullptr);
  }

  // Copy (both allocating and into destination) and reset.
  time_t* copied = static_cast<time_t*>(
      H5O_msg_copy(H5O_MTIME_NEW_ID, &kTimestamp, nullptr));
  ASSERT_NE(copied, nullptr);
  EXPECT_EQ(*copied, kTimestamp);
  H5O_msg_free(H5O_MTIME_NEW_ID, copied);

  time_t stack_copy = 0;
  EXPECT_EQ(H5O_msg_copy(H5O_MTIME_NEW_ID, &kTimestamp, &stack_copy),
            &stack_copy);
  EXPECT_EQ(stack_copy, kTimestamp);
  EXPECT_GE(H5O_msg_reset(H5O_MTIME_NEW_ID, &stack_copy), 0);
  EXPECT_EQ(stack_copy, 0);

  FILE* fp = std::tmpfile();
  ASSERT_NE(fp, nullptr);
  absl::Cleanup close_fp = [fp] { std::fclose(fp); };
  ASSERT_GE(H5O_debug_id(H5O_MTIME_NEW_ID, f, &kTimestamp, fp, 1, 10), 0);
  EXPECT_THAT(ReadTmpFile(fp), HasSubstr("Time:"));
}

TEST(H5OmtimeTest, LegacyMtimeEncodeDecodeValidationAndMakeTimeError) {
  ScopedFile file = CreateInMemoryFile("mtime_old_codec.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  const time_t kTimestamp = 1704164645;  // 2024-01-02 03:04:05 UTC
  EXPECT_EQ(H5O_msg_raw_size(f, H5O_MTIME_ID, false, &kTimestamp), 16U);

  std::array<uint8_t, 16> encoded{};
  ASSERT_GE(H5O_msg_encode(f, H5O_MTIME_ID, false, encoded.data(), &kTimestamp),
            0);
  EXPECT_STREQ(reinterpret_cast<const char*>(encoded.data()), "20240102030405");

  time_t* decoded = static_cast<time_t*>(
      H5O_msg_decode(f, nullptr, H5O_MTIME_ID, encoded.size(), encoded.data()));
  ASSERT_NE(decoded, nullptr);
  EXPECT_EQ(*decoded, kTimestamp);
  H5O_msg_free(H5O_MTIME_ID, decoded);

  {
    ScopedErrorSilence silence;
    // Short buffer (< 16 bytes).
    const uint8_t kEmptyGuard[1] = {0};
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MTIME_ID, 0, kEmptyGuard),
              nullptr);
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MTIME_ID, 15, encoded.data()),
              nullptr);

    // Non-digit character.
    std::array<uint8_t, 16> bad_digit = encoded;
    bad_digit[7] = 'X';
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MTIME_ID, bad_digit.size(),
                             bad_digit.data()),
              nullptr);

    // 1969-12-31 23:59:59 UTC equals (time_t)-1, triggering H5_make_time error.
    const std::array<uint8_t, 16> kMinusOneUtc = {'1', '9', '6', '9', '1', '2',
                                                  '3', '1', '2', '3', '5', '9',
                                                  '5', '9', 0,   0};
    EXPECT_EQ(H5O_msg_decode(f, nullptr, H5O_MTIME_ID, kMinusOneUtc.size(),
                             kMinusOneUtc.data()),
              nullptr);
  }

  // Debug output for legacy message ID.
  FILE* fp = std::tmpfile();
  ASSERT_NE(fp, nullptr);
  absl::Cleanup close_fp = [fp] { std::fclose(fp); };
  ASSERT_GE(H5O_debug_id(H5O_MTIME_ID, f, &kTimestamp, fp, 0, 12), 0);
  EXPECT_THAT(ReadTmpFile(fp), HasSubstr("Time:"));
}

TEST(H5OmtimeTest, ObjectHeaderV1StoresAndReadsMtimeNewMessage) {
  TempFile tmp("h5omtime_v1_persist.h5");

  {
    ScopedPlist fapl_disk(H5Pcreate(H5P_FILE_ACCESS));
    ASSERT_TRUE(fapl_disk.valid());
    ASSERT_GE(H5Pset_libver_bounds(fapl_disk.get(), H5F_LIBVER_EARLIEST,
                                   H5F_LIBVER_V18),
              0);
    ScopedFile file(
        H5Fcreate(tmp.c_str(), H5F_ACC_TRUNC, H5P_DEFAULT, fapl_disk.get()));
    ASSERT_TRUE(file.valid());

    ScopedPlist dcpl(H5Pcreate(H5P_DATASET_CREATE));
    ASSERT_TRUE(dcpl.valid());
    ASSERT_GE(H5Pset_obj_track_times(dcpl.get(), true), 0);
    const hsize_t dims[1] = {4};
    ScopedSpace space(H5Screate_simple(1, dims, nullptr));
    ASSERT_TRUE(space.valid());
    ScopedDataset dset(H5Dcreate2(file.get(), "/timed_ds", H5T_NATIVE_INT32,
                                  space.get(), H5P_DEFAULT, dcpl.get(),
                                  H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());
  }

  {
    ScopedFile file(H5Fopen(tmp.c_str(), H5F_ACC_RDONLY, H5P_DEFAULT));
    ASSERT_TRUE(file.valid());
    ScopedDataset dset(H5Dopen2(file.get(), "/timed_ds", H5P_DEFAULT));
    ASSERT_TRUE(dset.valid());

    // On v1 object headers, H5O_MTIME_NEW_ID populates info.ctime while
    // info.mtime remains 0 (see H5Oint.c).
    H5O_info2_t info{};
    ASSERT_GE(H5Oget_info3(dset.get(), &info, H5O_INFO_TIME), 0);
    EXPECT_EQ(info.mtime, static_cast<time_t>(0));
    EXPECT_GT(info.ctime, static_cast<time_t>(0));

    H5O_loc_t* oloc = H5O_get_loc(dset.get());
    ASSERT_NE(oloc, nullptr);
    ScopedApiContext ctx;
    EXPECT_GT(H5O_msg_exists(oloc, H5O_MTIME_NEW_ID), 0);
    time_t on_disk = 0;
    ASSERT_NE(H5O_msg_read(oloc, H5O_MTIME_NEW_ID, &on_disk), nullptr);
    EXPECT_EQ(on_disk, info.ctime);
  }
}

}  // namespace
}  // namespace hdf5
