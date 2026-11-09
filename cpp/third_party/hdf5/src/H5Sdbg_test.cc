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
// Tests internal dataspace debugging output in third_party/hdf5/src/H5Sdbg.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Sdbg.c

#include "third_party/hdf5/src/hdf5.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

// clang-format off
// `:hdf5` compiles with -DNDEBUG; match internal struct layouts before
// including package/private headers.
#ifndef NDEBUG
#define H5SDBG_TEST_RESTORE_UNDEF_NDEBUG 1
#define NDEBUG 1
#endif
#include "third_party/hdf5/google/util/hdf5_internal_prelude.h"
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wkeyword-macro"
#pragma push_macro("try")
#ifdef try
#undef try
#endif
#define try hdf5_try_param
extern "C" {
#define H5S_FRIEND
#include "third_party/hdf5/src/H5Iprivate.h"
#include "third_party/hdf5/src/H5Spkg.h"
#include "third_party/hdf5/src/H5VLprivate.h"
}  // extern "C"
#pragma pop_macro("try")
#pragma clang diagnostic pop
#ifdef H5SDBG_TEST_RESTORE_UNDEF_NDEBUG
#undef NDEBUG
#undef H5SDBG_TEST_RESTORE_UNDEF_NDEBUG
#endif
// clang-format on

namespace hdf5 {
namespace {

using ::testing::HasSubstr;

H5F_t* UnwrapFile(hid_t file_id) {
  return static_cast<H5F_t*>(H5VL_object_verify(file_id, H5I_FILE));
}

const H5S_t* UnwrapSpace(hid_t space_id) {
  return static_cast<const H5S_t*>(H5I_object_verify(space_id, H5I_DATASPACE));
}

std::string CaptureDebug(H5F_t* f, const H5S_t* space, int indent, int fwidth) {
  char* buf = nullptr;
  size_t len = 0;
  FILE* stream = open_memstream(&buf, &len);
  if (stream == nullptr) {
    return "";
  }
  absl::Cleanup free_buf = [&] { std::free(buf); };
  const herr_t rc = H5S_debug(f, space, stream, indent, fwidth);
  std::fclose(stream);
  if (rc < 0) {
    return "";
  }
  return std::string(buf != nullptr ? buf : "", len);
}

TEST(H5SdbgTest, FormatsNullScalarAndSimpleExtents) {
  ScopedFile file = CreateInMemoryFile("h5sdbg_basic.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(null_space.valid());
  const H5S_t* null_ptr = UnwrapSpace(null_space.get());
  ASSERT_NE(null_ptr, nullptr);
  const std::string null_out = CaptureDebug(f, null_ptr, 0, 20);
  EXPECT_THAT(null_out, HasSubstr("Space class:"));
  EXPECT_THAT(null_out, HasSubstr("H5S_NULL"));

  ScopedSpace scalar_space(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar_space.valid());
  const H5S_t* scalar_ptr = UnwrapSpace(scalar_space.get());
  ASSERT_NE(scalar_ptr, nullptr);
  const std::string scalar_out = CaptureDebug(f, scalar_ptr, 2, 18);
  EXPECT_THAT(scalar_out, HasSubstr("Space class:"));
  EXPECT_THAT(scalar_out, HasSubstr("H5S_SCALAR"));

  const hsize_t dims[2] = {4, 5};
  const hsize_t maxdims[2] = {8, H5S_UNLIMITED};
  ScopedSpace simple_space(H5Screate_simple(2, dims, maxdims));
  ASSERT_TRUE(simple_space.valid());
  const H5S_t* simple_ptr = UnwrapSpace(simple_space.get());
  ASSERT_NE(simple_ptr, nullptr);

  const std::string simple_out = CaptureDebug(f, simple_ptr, 2, 24);
  EXPECT_THAT(simple_out, HasSubstr("H5S_SIMPLE"));
  EXPECT_THAT(simple_out, HasSubstr("Rank:"));
  EXPECT_THAT(simple_out, HasSubstr("2"));

  // Exercise small fwidth clamping (`MAX(0, fwidth - 3)`).
  const std::string narrow_out = CaptureDebug(f, simple_ptr, 0, 1);
  EXPECT_THAT(narrow_out, HasSubstr("H5S_SIMPLE"));
}

TEST(H5SdbgTest, FormatsUnknownExtentClasses) {
  ScopedFile file = CreateInMemoryFile("h5sdbg_unknown.h5");
  ASSERT_TRUE(file.valid());
  H5F_t* f = UnwrapFile(file.get());
  ASSERT_NE(f, nullptr);

  H5S_t no_class_space{};
  no_class_space.extent.type = H5S_NO_CLASS;
  EXPECT_THAT(CaptureDebug(f, &no_class_space, 0, 20),
              HasSubstr("**UNKNOWN--1**"));

  H5S_t bogus_space{};
  bogus_space.extent.type = static_cast<H5S_class_t>(99);
  EXPECT_THAT(CaptureDebug(f, &bogus_space, 1, 15),
              HasSubstr("**UNKNOWN-99**"));
}

}  // namespace
}  // namespace hdf5
