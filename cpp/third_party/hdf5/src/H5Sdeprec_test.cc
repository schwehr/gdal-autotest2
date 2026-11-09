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
// Tests deprecated dataspace encoding APIs in third_party/hdf5/src/H5Sdeprec.c.
//
// See also:
//   https://github.com/HDFGroup/hdf5/blob/master/src/H5Sdeprec.c

#include "third_party/hdf5/src/hdf5.h"

#include <cstddef>
#include <vector>

#include "testing/base/public/gunit.h"
#include "third_party/hdf5/google/util/scoped_hdf5.h"

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define H5SDEPREC_TEST_HAS_ASAN 1
#endif
#endif
#if !defined(H5SDEPREC_TEST_HAS_ASAN) && \
    (defined(ADDRESS_SANITIZER) || defined(__SANITIZE_ADDRESS__))
#define H5SDEPREC_TEST_HAS_ASAN 1
#endif

namespace hdf5 {
namespace {

TEST(H5SdeprecTest, Encode1SizeProbeAndRoundTripAcrossTypes) {
  const hsize_t dims[2] = {4, 6};
  const hsize_t maxdims[2] = {8, H5S_UNLIMITED};
  const hsize_t start[2] = {1, 2};
  const hsize_t count[2] = {2, 3};

  ScopedSpace simple(H5Screate_simple(2, dims, maxdims));
  ASSERT_TRUE(simple.valid());
  ASSERT_GE(
      H5Sselect_hyperslab(simple.get(), H5S_SELECT_SET, start, nullptr, count,
                          nullptr),
      0);

  ScopedSpace scalar(H5Screate(H5S_SCALAR));
  ASSERT_TRUE(scalar.valid());

  ScopedSpace null_space(H5Screate(H5S_NULL));
  ASSERT_TRUE(null_space.valid());

  for (const hid_t space_id : {simple.get(), scalar.get(), null_space.get()}) {
    size_t nalloc = 0;
    ASSERT_GE(H5Sencode1(space_id, nullptr, &nalloc), 0);
    ASSERT_GT(nalloc, 0U);

    std::vector<unsigned char> buf(nalloc, 0);
    size_t actual_size = nalloc;
    ASSERT_GE(H5Sencode1(space_id, buf.data(), &actual_size), 0);
    EXPECT_EQ(actual_size, nalloc);

#if !defined(H5SDEPREC_TEST_HAS_ASAN)
    // Upstream H5S_decode calls H5S_select_deserialize(..., SIZE_MAX), where
    // `*p + SIZE_MAX - 1` in H5Sselect.c triggers UBSan pointer-overflow under
    // `--config=asan-chlor`.
    ScopedSpace decoded(H5Sdecode(buf.data()));
    ASSERT_TRUE(decoded.valid());
    EXPECT_GT(H5Sextent_equal(space_id, decoded.get()), 0);
    EXPECT_EQ(H5Sget_select_npoints(decoded.get()),
              H5Sget_select_npoints(space_id));
#endif
  }
}

TEST(H5SdeprecTest, Encode1RejectsInvalidObjectAndUnencodableSelection) {
  ScopedErrorSilence silence;
  size_t nalloc = 0;
  EXPECT_LT(H5Sencode1(H5I_INVALID_HID, nullptr, &nalloc), 0);

  ScopedFile file = CreateInMemoryFile("h5sdeprec_invalid.h5");
  ASSERT_TRUE(file.valid());
  EXPECT_LT(H5Sencode1(file.get(), nullptr, &nalloc), 0);

  // Negative selection offset moving bounds below zero makes H5S_encode fail.
  const hsize_t dims[1] = {10};
  const hsize_t coord[1] = {1};
  const hssize_t neg_offset[1] = {-5};
  ScopedSpace space(H5Screate_simple(1, dims, nullptr));
  ASSERT_TRUE(space.valid());
  ASSERT_GE(H5Sselect_elements(space.get(), H5S_SELECT_SET, 1, coord), 0);
  ASSERT_GE(H5Soffset_simple(space.get(), neg_offset), 0);

  std::vector<unsigned char> buf(256, 0);
  nalloc = buf.size();
  EXPECT_LT(H5Sencode1(space.get(), buf.data(), &nalloc), 0);
}

}  // namespace
}  // namespace hdf5
