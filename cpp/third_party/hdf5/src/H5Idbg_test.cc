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
// Tests internal H5I debugging dump routines (third_party/hdf5/src/H5Idbg.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Idbg.c

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <array>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"
#include "third_party/hdf5/google/util/temp_file.h"
#include "third_party/hdf5/src/hdf5.h"

#ifndef NDEBUG
#define H5I_DBG_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
extern "C" {
#include "third_party/hdf5/src/H5Iprivate.h"
}  // extern "C"
#ifdef H5I_DBG_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5I_DBG_TEST_RESTORE_UNDEF_NDEBUG
#endif

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

// Redirects STDERR_FILENO into a temporary file so large ID dumps cannot block
// on pipe buffers and all output stays silent in test logs.
class ScopedStderrCapture {
 public:
  explicit ScopedStderrCapture(const char* name) : temp_(name) {
    std::fflush(stderr);
    saved_fd_ = dup(STDERR_FILENO);
    capture_fd_ = open(temp_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (saved_fd_ >= 0 && capture_fd_ >= 0) {
      redirected_ = (dup2(capture_fd_, STDERR_FILENO) >= 0);
    }
  }

  ~ScopedStderrCapture() { Restore(); }

  std::string FinishAndRead() {
    Restore();
    std::ifstream in(temp_.c_str(), std::ios::in | std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
  }

  ScopedStderrCapture(const ScopedStderrCapture&) = delete;
  ScopedStderrCapture& operator=(const ScopedStderrCapture&) = delete;

 private:
  void Restore() {
    if (redirected_) {
      std::fflush(stderr);
      dup2(saved_fd_, STDERR_FILENO);
      redirected_ = false;
    }
    if (capture_fd_ >= 0) {
      close(capture_fd_);
      capture_fd_ = -1;
    }
    if (saved_fd_ >= 0) {
      close(saved_fd_);
      saved_fd_ = -1;
    }
  }

  TempFile temp_;
  int saved_fd_ = -1;
  int capture_fd_ = -1;
  bool redirected_ = false;
};

TEST(H5IdbgTest, DumpIdsCoversLibraryAndUserTypesAndPaths) {
  ScopedFile file = CreateInMemoryFile("h5idbg_types.h5");
  ASSERT_TRUE(file.valid());

  ScopedGroup group(
      H5Gcreate2(file.get(), "/dbg_grp", H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT));
  ASSERT_TRUE(group.valid());

  constexpr std::array<hsize_t, 1> kDims = {4};
  ScopedSpace space(H5Screate_simple(1, kDims.data(), nullptr));
  ASSERT_TRUE(space.valid());

  ScopedDataset dataset(H5Dcreate2(file.get(), "/dbg_grp/dset", H5T_NATIVE_INT,
                                   space.get(), H5P_DEFAULT, H5P_DEFAULT,
                                   H5P_DEFAULT));
  ASSERT_TRUE(dataset.valid());

  ScopedType transient_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(transient_dt.valid());

  ScopedType committed_dt(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(committed_dt.valid());
  ASSERT_GE(H5Tcommit2(file.get(), "/dbg_committed_dt", committed_dt.get(),
                      H5P_DEFAULT, H5P_DEFAULT, H5P_DEFAULT),
            0);

  const H5I_type_t destroyed_type = H5Iregister_type2(/*reserved=*/0, nullptr);
  ASSERT_GE(destroyed_type, H5I_NTYPES);
  ASSERT_GE(H5Idestroy_type(destroyed_type), 0);

  const H5I_type_t empty_type = H5Iregister_type2(/*reserved=*/2, nullptr);
  ASSERT_GE(empty_type, H5I_NTYPES);
  absl::Cleanup destroy_empty = [empty_type] { H5Idestroy_type(empty_type); };

  const H5I_type_t active_type = H5Iregister_type2(/*reserved=*/3, nullptr);
  ASSERT_GE(active_type, H5I_NTYPES);
  absl::Cleanup destroy_active = [active_type] { H5Idestroy_type(active_type); };

  int32_t payload = 1234;
  const hid_t custom_id = H5Iregister(active_type, &payload);
  ASSERT_GE(custom_id, 0);

  // H5T_nameof emits an error on transient datatypes inside H5I__id_dump_cb.
  ScopedErrorSilence silence;
  ScopedStderrCapture capture("h5idbg_dump.txt");

  EXPECT_GE(H5I_dump_ids_for_type(destroyed_type), 0);
  EXPECT_GE(H5I_dump_ids_for_type(H5I_MAP), 0);
  EXPECT_GE(H5I_dump_ids_for_type(empty_type), 0);
  EXPECT_GE(H5I_dump_ids_for_type(active_type), 0);
  EXPECT_GE(H5I_dump_ids_for_type(H5I_FILE), 0);
  EXPECT_GE(H5I_dump_ids_for_type(H5I_GROUP), 0);
  EXPECT_GE(H5I_dump_ids_for_type(H5I_DATASET), 0);
  EXPECT_GE(H5I_dump_ids_for_type(H5I_DATATYPE), 0);
  EXPECT_GE(H5I_dump_ids_for_type(H5I_DATASPACE), 0);

  const std::string output = capture.FinishAndRead();
  EXPECT_THAT(output, HasSubstr("Global type info/tracking pointer for that type is NULL"));
  EXPECT_THAT(output, HasSubstr("reserved   = 2"));
  EXPECT_THAT(output, HasSubstr("id_count   = 1"));
  EXPECT_THAT(output, HasSubstr("(HASH TABLE)"));
  EXPECT_THAT(output, HasSubstr("user_path = /dbg_committed_dt"));
  EXPECT_THAT(output, HasSubstr("full_path = /dbg_committed_dt"));
}

}  // namespace
}  // namespace hdf5
