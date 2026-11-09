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
// Tests opaque datatype tag assignment and retrieval (`H5Tset_tag`,
// `H5Tget_tag`) across transient, derived, immutable, and committed opaque
// datatypes (third_party/hdf5/src/H5Topaque.c).
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Topaque.c

#include "third_party/hdf5/src/hdf5.h"

#include <cstddef>
#include <string>

#include "testing/base/public/gmock.h"
#include "testing/base/public/gunit.h"
#include "third_party/absl/cleanup/cleanup.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

namespace hdf5 {
namespace {

std::string ReadOpaqueTag(hid_t type_id) {
  char* raw = H5Tget_tag(type_id);
  if (raw == nullptr) {
    return "<null>";
  }
  absl::Cleanup free_raw = [raw] { H5free_memory(raw); };
  return std::string(raw);
}

TEST(H5TopaqueTest, SetAndGetTagRoundTripAndOverwrite) {
  ScopedType opaque(H5Tcreate(H5T_OPAQUE, 16));
  ASSERT_TRUE(opaque.valid());

  // Newly created opaque datatypes default to an empty tag string.
  EXPECT_EQ(ReadOpaqueTag(opaque.get()), "");

  constexpr char kTag1[] = "pixel_rgba8";
  ASSERT_GE(H5Tset_tag(opaque.get(), kTag1), 0);
  EXPECT_EQ(ReadOpaqueTag(opaque.get()), kTag1);

  // Setting a new tag frees and replaces the previous tag.
  constexpr char kTag2[] = "sensor_frame_v2";
  ASSERT_GE(H5Tset_tag(opaque.get(), kTag2), 0);
  EXPECT_EQ(ReadOpaqueTag(opaque.get()), kTag2);

  // Empty string is a valid non-null tag.
  constexpr char kEmptyTag[] = "";
  ASSERT_GE(H5Tset_tag(opaque.get(), kEmptyTag), 0);
  EXPECT_EQ(ReadOpaqueTag(opaque.get()), kEmptyTag);
}

TEST(H5TopaqueTest, TagLengthBoundaryRespectsOpaqueTagMax) {
  ScopedType opaque(H5Tcreate(H5T_OPAQUE, 8));
  ASSERT_TRUE(opaque.valid());

  const std::string max_valid(H5T_OPAQUE_TAG_MAX - 1, 'a');
  ASSERT_GE(H5Tset_tag(opaque.get(), max_valid.c_str()), 0);
  EXPECT_EQ(ReadOpaqueTag(opaque.get()), max_valid);

  // Exactly H5T_OPAQUE_TAG_MAX characters exceeds the NUL-included limit.
  ScopedErrorSilence silence;
  const std::string too_long(H5T_OPAQUE_TAG_MAX, 'b');
  EXPECT_LT(H5Tset_tag(opaque.get(), too_long.c_str()), 0);
  EXPECT_EQ(ReadOpaqueTag(opaque.get()), max_valid);
}

TEST(H5TopaqueTest, DerivedArrayAndVlenDeferToOpaqueParent) {
  ScopedType opaque(H5Tcreate(H5T_OPAQUE, 4));
  ASSERT_TRUE(opaque.valid());
  ASSERT_GE(H5Tset_tag(opaque.get(), "base_tag"), 0);

  constexpr hsize_t kDims[1] = {3};
  ScopedType arr(H5Tarray_create2(opaque.get(), 1, kDims));
  ASSERT_TRUE(arr.valid());
  EXPECT_EQ(ReadOpaqueTag(arr.get()), "base_tag");

  ScopedType vlen(H5Tvlen_create(arr.get()));
  ASSERT_TRUE(vlen.valid());
  EXPECT_EQ(ReadOpaqueTag(vlen.get()), "base_tag");

  // H5Tarray_create2 and H5Tvlen_create copy their base datatype, so `vlen` has
  // its own independent ancestor chain from `arr` and `opaque`. Calling
  // H5Tset_tag walks those parent pointers to the root opaque datatype.
  ASSERT_GE(H5Tset_tag(vlen.get(), "updated_via_vlen"), 0);
  EXPECT_EQ(ReadOpaqueTag(vlen.get()), "updated_via_vlen");
  EXPECT_EQ(ReadOpaqueTag(arr.get()), "base_tag");
  EXPECT_EQ(ReadOpaqueTag(opaque.get()), "base_tag");

  ASSERT_GE(H5Tset_tag(arr.get(), "updated_via_arr"), 0);
  EXPECT_EQ(ReadOpaqueTag(arr.get()), "updated_via_arr");
  EXPECT_EQ(ReadOpaqueTag(opaque.get()), "base_tag");
}

TEST(H5TopaqueTest, PredefinedLockedReadOnlyAndCommittedTagBehavior) {
  // Predefined H5T_NATIVE_OPAQUE is immutable.
  EXPECT_EQ(ReadOpaqueTag(H5T_NATIVE_OPAQUE), "");
  {
    ScopedErrorSilence silence;
    EXPECT_LT(H5Tset_tag(H5T_NATIVE_OPAQUE, "nope"), 0);
  }

  // Locked transient opaque datatype is immutable (also unclosable until exit).
  {
    ScopedErrorSilence silence;
    ScopedType locked(H5Tcreate(H5T_OPAQUE, 12));
    ASSERT_TRUE(locked.valid());
    ASSERT_GE(H5Tset_tag(locked.get(), "locked_tag"), 0);
    ASSERT_GE(H5Tlock(locked.get()), 0);
    EXPECT_EQ(ReadOpaqueTag(locked.get()), "locked_tag");
    EXPECT_LT(H5Tset_tag(locked.get(), "mutated"), 0);
    EXPECT_EQ(ReadOpaqueTag(locked.get()), "locked_tag");
  }

  // Committed opaque datatype persisted in an in-memory file preserves its tag.
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());
  {
    ScopedType to_commit(H5Tcreate(H5T_OPAQUE, 24));
    ASSERT_TRUE(to_commit.valid());
    ASSERT_GE(H5Tset_tag(to_commit.get(), "persisted_opaque"), 0);
    ASSERT_GE(H5Tcommit2(file.get(), "opaque_dt", to_commit.get(), H5P_DEFAULT,
                        H5P_DEFAULT, H5P_DEFAULT),
              0);
  }

  ScopedType reopened(H5Topen2(file.get(), "opaque_dt", H5P_DEFAULT));
  ASSERT_TRUE(reopened.valid());
  EXPECT_EQ(ReadOpaqueTag(reopened.get()), "persisted_opaque");
}

TEST(H5TopaqueTest, InvalidArgsAndNonOpaqueTypesFail) {
  ScopedFile file = CreateInMemoryFile();
  ASSERT_TRUE(file.valid());

  ScopedType opaque(H5Tcreate(H5T_OPAQUE, 8));
  ASSERT_TRUE(opaque.valid());

  ScopedType int_copy(H5Tcopy(H5T_NATIVE_INT));
  ASSERT_TRUE(int_copy.valid());

  constexpr hsize_t kDims[1] = {2};
  ScopedType int_arr(H5Tarray_create2(int_copy.get(), 1, kDims));
  ASSERT_TRUE(int_arr.valid());

  ScopedErrorSilence silence;

  // Invalid or non-datatype IDs.
  EXPECT_LT(H5Tset_tag(H5I_INVALID_HID, "tag"), 0);
  EXPECT_LT(H5Tset_tag(file.get(), "tag"), 0);
  EXPECT_EQ(H5Tget_tag(H5I_INVALID_HID), nullptr);
  EXPECT_EQ(H5Tget_tag(file.get()), nullptr);

  // Non-opaque datatypes (including derived types over non-opaque parents).
  EXPECT_LT(H5Tset_tag(int_copy.get(), "tag"), 0);
  EXPECT_LT(H5Tset_tag(int_arr.get(), "tag"), 0);
  EXPECT_EQ(H5Tget_tag(H5T_NATIVE_INT), nullptr);
  EXPECT_EQ(H5Tget_tag(int_arr.get()), nullptr);

  // Null tag pointer.
  EXPECT_LT(H5Tset_tag(opaque.get(), nullptr), 0);
}

}  // namespace
}  // namespace hdf5
